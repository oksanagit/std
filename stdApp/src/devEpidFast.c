/* devEpidMpf.cc

    Author: Mark Rivers
    Date: 10/11/99
    9-July-2004  Converted from MPF to asyn and from C++ to C

*/


#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
 
#include <dbAccess.h>
#include <dbDefs.h>
#include <link.h>
#include <epicsPrint.h>
#include <epicsExport.h>
#include <epicsMutex.h>
#include <epicsString.h>
#include <dbCommon.h>
#include <recSup.h>
#include <devSup.h>
#include <recGbl.h>
#include <cantProceed.h>
#include <alarm.h>
#include <asynDriver.h>
#include <asynFloat64.h>
#include <asynFloat64Callback.h>

#include "epidRecord.h"

typedef struct {
    double setPoint;
    double actual;
    double error;
    double prevError;
    int feedbackOn;
    int prevFeedbackOn;
    double highLimit;
    double lowLimit;
    double output;
    double KP; 
    double KI; 
    double KD;
    double P;
    double I;
    double D;
    double callbackInterval;
    double timePerPointRequested;
    double timePerPointActual;
    asynFloat64Callback *pinput;
    void *inputPvt;
    int inputChannel;
    char *inputName;
    asynFloat64 *poutput;
    void *outputPvt;
    int outputChannel;
    char *outputName;
    asynUser *pinputAsynUser;
    asynUser *poutputAsynUser;
    double averageStore;
    int numAverage;
    int accumulated;
    epicsMutexId mutexId;
} epidFastPvt;

/* These functions are called from the record */
static long init_record(epidRecord *pepid);
static long update_params(epidRecord *epid);
/* These are private functions */
static void computeNumAverage(epidFastPvt *pPvt);
static void do_PID(epidFastPvt *pPvt, double readback);
/* These are callback functions, called from the driver */
static void dataCallback(void *drvPvt, double readback);
static void intervalCallback(void *drvPvt, double seconds);

typedef struct {
    long            number;
    DEVSUPFUN       report;
    DEVSUPFUN       init;
    DEVSUPFUN       init_record;
    DEVSUPFUN       get_ioint_info;
    DEVSUPFUN       update_params;
} epidFastDset;

epidFastDset devEpidFast = {
    6,
    NULL,
    NULL,
    init_record,
    NULL,
    update_params
};
epicsExportAddress(dset, devEpidFast);

static long init_record(epidRecord *pepid)
{
    struct vmeio *pvmeio;
    asynStatus status;
    asynUser *pasynUser;
    asynInterface *pasynInterface;
    epidFastPvt *pPvt;
    char *tok_save;
    char *p;
    char temp[100];

    pPvt = callocMustSucceed(1, sizeof(*pPvt), "devEpidFast::init_record");
    pepid->dpvt = pPvt;
    pPvt->KP = 1;
    pPvt->lowLimit = 1.;
    pPvt->highLimit =-1.;

    /* Get the VME link field */
    /* Get the signal from the VME signal */
    pvmeio = (struct vmeio*)&(pepid->inp.value);
    /* Parse the parm field to get inputName, inputChannel, 
     * outputName, outputChannel 
     * Copy parm to temp, since strtok_r overwrites it */
    strncpy(temp, pvmeio->parm, sizeof(temp));
    tok_save = NULL;
    p = strtok_r(temp, ", ", &tok_save);
    pPvt->inputName = epicsStrDup(p);
    p = strtok_r(NULL, ", ", &tok_save);
    pPvt->inputChannel = atoi(p);
    p = strtok_r(NULL, ", ", &tok_save);
    pPvt->outputName = epicsStrDup(p);
    p = strtok_r(NULL, ", ", &tok_save);
    pPvt->outputChannel = atoi(p);
    pPvt->mutexId = epicsMutexCreate();

    pasynUser = pasynManager->createAsynUser(0, 0);
    pPvt->pinputAsynUser = pasynUser;
    status = pasynManager->connectDevice(pasynUser, pPvt->inputName, 
                                         pPvt->inputChannel);
    if (status != asynSuccess) {
        errlogPrintf("devEpidFast::init_record, error in connectDevice"
                     " to input %s\n",
                     pasynUser->errorMessage);
        goto bad;
    }
    pasynInterface = pasynManager->findInterface(pasynUser,
                                                 asynFloat64CallbackType, 1);
    if (!pasynInterface) {
        errlogPrintf("devEpidFast::init_record, cannot find "
                     "asynFloat64Callback interface %s\n",
                     pasynUser->errorMessage);
        goto bad;
    }
    pPvt->pinput = (asynFloat64Callback *)pasynInterface->pinterface;
    pPvt->inputPvt = pasynInterface->drvPvt;

    pasynUser = pasynManager->createAsynUser(0, 0);
    pPvt->poutputAsynUser = pasynUser;
    status = pasynManager->connectDevice(pasynUser, pPvt->outputName, 
                                         pPvt->outputChannel);
    if (status != asynSuccess) {
        errlogPrintf("devEpidFast::init_record, error in connectDevice"
                     " to output %s\n",
                     pasynUser->errorMessage);
        goto bad;
    }
    pasynInterface = pasynManager->findInterface(pasynUser,
                                                 asynFloat64Type, 1);
    if (!pasynInterface) {
        errlogPrintf("devEpidFast::init_record, cannot find "
                     "asynFloat64 interface %s\n",
                     pasynUser->errorMessage);
        goto bad;
    }
    pPvt->poutput = (asynFloat64 *)pasynInterface->pinterface;
    pPvt->outputPvt = pasynInterface->drvPvt;

    pPvt->pinput->registerCallbacks(pPvt->inputPvt, pPvt->pinputAsynUser,
                                   dataCallback, intervalCallback, pPvt);
    pPvt->callbackInterval = pPvt->pinput->getCallbackInterval(pPvt->inputPvt,
                                                 pPvt->pinputAsynUser);
    update_params(pepid);
    return(0);
bad:
    pepid->pact=1;
    return(0);
}


static long update_params(epidRecord *pepid)
{
    epidFastPvt *pPvt = (epidFastPvt *)pepid->dpvt;

    epicsMutexLock(pPvt->mutexId);
    /* If the user has changed the value of dt, requested time per point, 
     * then update numAverage */
    if (pepid->dt != pPvt->timePerPointActual) {
        pPvt->timePerPointRequested = pepid->dt;
        computeNumAverage(pPvt);
    }
    /* Copy values from private structure to record */
    pepid->cval = pPvt->actual;
    pepid->err  = pPvt->error;
    pepid->oval = pPvt->output;
    pepid->p    = pPvt->P;
    pepid->i    = pPvt->I;
    pepid->d    = pPvt->D;
    pepid->dt   = pPvt->timePerPointActual;

    /* Copy values from record to private structure */
    pPvt->feedbackOn = pepid->fbon;
    pPvt->highLimit = pepid->drvh;
    pPvt->lowLimit = pepid->drvl;
    pPvt->KP = pepid->kp;
    pPvt->KI = pepid->ki;
    pPvt->KD = pepid->kd;
    pPvt->setPoint = pepid->val;
    epicsMutexUnlock(pPvt->mutexId);

    asynPrint(pPvt->pinputAsynUser, ASYN_TRACEIO_DEVICE,
              "devEpidFast::update_params, record=%s\n "
              "    feedback state %d, previous feedback state %d\n"
              "    cval=%f, err=%f, oval=%f,\n" 
              "    P=%f, I=%f, D=%f, dt=%f\n", 
              pepid->name, 
              pPvt->feedbackOn, pPvt->prevFeedbackOn,
              pepid->cval, pepid->err, pepid->oval,
              pepid->p, pepid->i, pepid->d, pepid->dt);

    pepid->udf=0;
    return(0);
}

static void computeNumAverage(epidFastPvt *pPvt)
{
    pPvt->numAverage = 0.5 + pPvt->timePerPointRequested / 
                             pPvt->callbackInterval;
    if (pPvt->numAverage < 1) pPvt->numAverage = 1;
    pPvt->timePerPointActual = pPvt->numAverage * pPvt->callbackInterval;
}


/* This is the function that is called back from the driver when the time
 * interval changes */
static void intervalCallback(void *drvPvt, double seconds)
{
    epidFastPvt *pPvt = (epidFastPvt *)drvPvt;

    epicsMutexLock(pPvt->mutexId);
    pPvt->callbackInterval = seconds;
    computeNumAverage(pPvt);
    epicsMutexUnlock(pPvt->mutexId);
}

/* This is the function that is called back from the driver when a new readback
 * value is obtained */
static void dataCallback(void *drvPvt, double readBack)
{
    epidFastPvt *pPvt = (epidFastPvt *)drvPvt;

    epicsMutexLock(pPvt->mutexId);
    /* No need to average if collecting every point */
    if (pPvt->numAverage == 1) {
        do_PID(pPvt, readBack);
        goto done;
    }
    pPvt->averageStore += readBack;
    if (++pPvt->accumulated < pPvt->numAverage) goto done;
    /* We have now collected the desired number of points to average */
    pPvt->averageStore /= pPvt->accumulated;
    do_PID(pPvt, pPvt->averageStore);
    pPvt->averageStore = 0.;
    pPvt->accumulated = 0;
done:
    epicsMutexUnlock(pPvt->mutexId);
}

static void do_PID(epidFastPvt *pPvt, double readBack)

/*
    04/20/00  MLR  Changed timing logic.  No longer reprogram IP-330 clock,
                   just change whether the callback is executed when called.
    05/02/00  MLR  Fixed bug in time calculation for integral term
    05/03/00  MLR  Added more sanity checks to integral term:
                   If KI is 0. then set I to DRVL if KP is greater than 0,
                   set I to DRVH is KP is less than 0.
                   If feedback is off don't change integral term.
    05/17/00  MLR  Added another sanity checks to integral term:
                   When feedback goes from OFF to ON then set integral term
                   equal to the current value of the DAC output.  This ensures
                   a "bumpless" turn-on of feedback
    01/16/01 MLR   Added check for valid pFast in fastPID::init
    04/08/03 MLR   Converted to abstract base class, fastPID, from Ip300PID.
                   fastPID is device-independent.  This class must be derived
                   by a device-dependent class that implements writeOutput(),
                   readOutput(), and periodically calls doPID(), typically when
                   a new input value is available.
                   Converted Ip330PIDServer to base class fastPIDServer,
                   moved to this file.
    07/09/04 MLR   Converted from MPF to asyn, and from C++ to C
*/
{
    double dt;
    double derror;
    double dI;

    dt = pPvt->callbackInterval;
    pPvt->actual = readBack;
    pPvt->prevError = pPvt->error;
    pPvt->error = pPvt->setPoint - pPvt->actual;
    derror = pPvt->error - pPvt->prevError;
    pPvt->P = pPvt->KP*pPvt->error;
    /* Sanity checks on integral term:
        * 1) Don't increase I if output >= highLimit
        * 2) Don't decrease I if output <= lowLimit
        * 3) Don't change I if feedback is off
        * 4) Limit the integral term to be in the range betweem DRLV and DRVH
        * 5) If KI is zero then set the sum to DRVL (if KI is positive), or
        *    DRVH (if KP is negative) to allow easily turning off the
        *    integral term for PID tuning.
    */
    dI = pPvt->KP*pPvt->KI*pPvt->error*dt;
    if (pPvt->feedbackOn) {
        if (!pPvt->prevFeedbackOn) {
            pPvt->poutput->read(pPvt->outputPvt, pPvt->poutputAsynUser,
                                &pPvt->I);
        } else {
            if (((pPvt->output > pPvt->lowLimit) &&
                 (pPvt->output < pPvt->highLimit)) ||
                ((pPvt->output >= pPvt->highLimit) && ( dI < 0.)) ||
                ((pPvt->output <= pPvt->lowLimit)  && ( dI > 0.))) {
                pPvt->I = pPvt->I + dI;
                if (pPvt->I < pPvt->lowLimit) pPvt->I = pPvt->lowLimit;
                if (pPvt->I > pPvt->highLimit) pPvt->I = pPvt->highLimit;
            }
        }
    }
    if (pPvt->KI == 0.) {
        if (pPvt->KP > 0.) pPvt->I = pPvt->lowLimit;
        else pPvt->I = pPvt->highLimit;
    }
    if (dt>0.0) pPvt->D = pPvt->KP*pPvt->KD*(derror/dt); else pPvt->D = 0.0;
    pPvt->output = (pPvt->P + pPvt->I + pPvt->D);
    /* Limit output to range from low to high limit */
    if (pPvt->output > pPvt->highLimit) pPvt->output = pPvt->highLimit;
    if (pPvt->output < pPvt->lowLimit) pPvt->output = pPvt->lowLimit;

    /* If feedback is on write output */
    if (pPvt->feedbackOn) {
        pPvt->poutput->write(pPvt->outputPvt, pPvt->poutputAsynUser,
                             pPvt->output);
    }
    /* Save state of feedback */
    pPvt->prevFeedbackOn = pPvt->feedbackOn;
}