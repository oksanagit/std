/*
 *
 *      Author:	Tim Mooney
 *      Date:	05-17-99
 *
 *      Experimental Physics and Industrial Control System (EPICS)
 *
 *      Copyright 1999, the Regents of the University of California,
 *      and the University of Chicago Board of Governors.
 *
 *      This software was produced under  U.S. Government contracts:
 *      (W-7405-ENG-36) at the Los Alamos National Laboratory,
 *      and (W-31-109-ENG-38) at Argonne National Laboratory.
 *
 *      Initial development by:
 *              The Controls and Automation Group (AT-8)
 *              Ground Test Accelerator
 *              Accelerator Technology Division
 *              Los Alamos National Laboratory
 *
 *      Co-developed with
 *              The Beamline Controls & Data Acquisition Group,
 *                 Experimental Facilities Division; and
 *              The Controls and Computing Group,
 *                 Accelerator Systems Division
 *              Advanced Photon Source
 *              Argonne National Laboratory
 *
 * Modification Log:
 * -----------------
 *  .01 05-17-99  tmm  Created from seq record (by John Winans)
 */
#include	<vxWorks.h>
#include	<stdlib.h>
#include	<stdio.h>
#include	<string.h>
#include	<lstLib.h>
#include	<string.h>
#include	<memLib.h>
#include	<wdLib.h>
#include	<sysLib.h>

#include	<dbDefs.h>
#include	<epicsPrint.h>
#include	<alarm.h>
#include	<dbAccess.h>
#include	<dbEvent.h>
#include	<dbScan.h>
#include	<dbFldTypes.h>
#include	<devSup.h>
#include	<errMdef.h>
#include	<recSup.h>
#include	<special.h>
#include	<callback.h>
#include	<cvtFast.h>

#define GEN_SIZE_OFFSET
#include	"sseqRecord.h"
#undef  GEN_SIZE_OFFSET

int	sseqRecDebug = 0;


/* Total number of link-groups in a sequence record */
#define NUM_LINKS	10
#define SELN_BIT_MASK	~(0xffff << NUM_LINKS)

#define DBF_unknown -1
/* This is what a link-group looks like in a string-sequence record */
struct	linkDesc {
	double          dly;	/* Delay value (in seconds) */
	struct link     dol;	/* Where to fetch the input value from */
	double          dov;	/* If dol is CONSTANT, this is the CONSTANT value */
	struct link     lnk;	/* Where to put the value from dol */
	char            s[40]; /* string value */
	short			dol_field_type;
	short			lnk_field_type;
};

/* Callback structure used by the watchdog function to queue link processing */
#define LINKS_ALL_OK	0
#define LINKS_NOT_OK	1
struct callbackSeq {
	CALLBACK		callback;	/* used for the callback task */
	WDOG_ID			wd_id;		/* Watchdog ID's for delays */
	struct linkDesc	*plinks[NUM_LINKS+1]; /* Pointers to links to process */
	int				index;
	CALLBACK		checkLinksCB;
	WDOG_ID			wd_id_1;
	short			wd_id_1_LOCK;
	short			linkStat; /* LINKS_ALL_OK, LINKS_NOT_OK */
};

static long init_record(sseqRecord *pR, int pass);
static long process(sseqRecord *pR);
static int processNextLink(sseqRecord *pR);
static long asyncFinish(sseqRecord *pR);
static void watchDog(CALLBACK *pcallback);
static void processCallback(CALLBACK *pCallback);
static long get_precision(struct dbAddr *paddr, long *precision);
static void checkLinksCallback(CALLBACK *pCallback);
static void checkLinks(sseqRecord *pR);
static long special(struct dbAddr *paddr, int after);

/* Create RSET - Record Support Entry Table*/
struct rset sseqRSET={
	RSETNUMBER,
	NULL,			/* report */
	NULL,			/* initialize */
	init_record,	/* init_record */
	process,		/* process */
	special,		/* special */
	NULL,			/* get_value */
	NULL,			/* cvt_dbaddr */
	NULL,			/* get_array_info */
	NULL,			/* put_array_info */
	NULL,			/* get_units */
	get_precision,	/* get_precision */
	NULL,			/* get_enum_str */
	NULL,			/* get_enum_strs */
	NULL,			/* put_enum_str */
	NULL,			/* get_graphic_double */
	NULL,			/* get_control_double */
	NULL 			/* get_alarm_double */

};
/*****************************************************************************
 *
 * Initialize a sequence record.
 *
 * Allocate the callback request structure (tacked on to dpvt.)
 * Initialize watch-dog ID
 * Initialize SELN based on the link-type of SELL
 * If SELL is a CA_LINK, inform CA about it
 * For each constant input link, fill in the DOV field
 *
 ******************************************************************************/
static long 
init_record(sseqRecord *pR, int pass)
{
	int					index;
	struct linkDesc		*plink;
	struct callbackSeq	*pdpvt;
	struct dbAddr       dbAddr;
	struct dbAddr       *pAddr = &dbAddr;

	if (pass==0) return(0);

	if (sseqRecDebug > 5) {
		printf("init_record(%s) entered\n", pR->name);
	}

	/* Allocate a callback structure for use in processing */
	pR->dpvt = (void *)malloc(sizeof(struct  callbackSeq));
	pdpvt = (struct callbackSeq *)pR->dpvt;

	callbackSetCallback(processCallback, &pdpvt->callback);
	callbackSetPriority(pR->prio, &pdpvt->callback);
	callbackSetUser(pR, &pdpvt->callback);
	pdpvt->wd_id = wdCreate();

	callbackSetCallback(checkLinksCallback, &pdpvt->checkLinksCB);
	callbackSetPriority(0, &pdpvt->checkLinksCB);
	callbackSetUser(pR, &pdpvt->checkLinksCB);
	pdpvt->wd_id_1 = wdCreate();
	pdpvt->wd_id_1_LOCK = 0;

	/* Get link selection if sell is a constant and nonzero */
	if (pR->sell.type==CONSTANT) {
		if (sseqRecDebug > 5) {
			printf("init_record(%s) SELL is a constant\n", pR->name);
		}
		recGblInitConstantLink(&pR->sell,DBF_USHORT,&pR->seln);
	}

	/*** init links, get initial values, field types ***/
	plink = (struct linkDesc *)(&(pR->dly1));
	for (index = 0; index < NUM_LINKS; index++, plink++) {
		/* init DOL*-related stuff (input links) */
		if (plink->dol.type == CONSTANT) {
			recGblInitConstantLink(&plink->dol, DBF_DOUBLE, &plink->dov);
			recGblInitConstantLink(&plink->dol, DBF_STRING, plink->s);
			plink->dol_field_type = DBF_NOACCESS;
        } else if (!dbNameToAddr(plink->dol.value.pv_link.pvname, pAddr)) {
			plink->dol_field_type = pAddr->field_type;
			if (sseqRecDebug) printf("sseq:init:dol_field_type=%d (%s)\n",
				plink->dol_field_type,
				pamapdbfType[plink->dol_field_type].strvalue);
		} else {
			/* pv is not on this ioc. Callback later for connection stat */
			pdpvt->linkStat = LINKS_NOT_OK;
			plink->dol_field_type = DBF_unknown; /* don't know field type */
		}
		/* same for LNK* stuff (output links) */
		if (plink->lnk.type == CONSTANT) {
			plink->lnk_field_type = DBF_unknown;
        } else if (!dbNameToAddr(plink->lnk.value.pv_link.pvname, pAddr)) {
			plink->lnk_field_type = pAddr->field_type;
			if (sseqRecDebug) printf("sseq:init:lnk_field_type=%d (%s)\n",
				plink->lnk_field_type,
				pamapdbfType[plink->lnk_field_type].strvalue);
		} else {
			/* pv is not on this ioc. Callback later for connection stat */
			pdpvt->linkStat = LINKS_NOT_OK;
			plink->lnk_field_type = DBF_unknown; /* don't know field type */
		}

		/* convert between value types */
		if (plink->s[0]) {
			plink->dov = atof(plink->s);
			db_post_events(pR, &plink->dov, DBE_VALUE);
		} else {
			cvtDoubleToString(plink->dov, plink->s, pR->prec);
			db_post_events(pR, &plink->s, DBE_VALUE);
		}
	}

	if (pdpvt->linkStat == LINKS_NOT_OK) {
		wdStart(pdpvt->wd_id_1, 60, (FUNCPTR)callbackRequest,
			(int)(&pdpvt->checkLinksCB));
		pdpvt->wd_id_1_LOCK = 1;
	}

	return(0);
}
/*****************************************************************************
 *
 * Process a sequence record.
 *
 * If is async completion phase
 *   call asyncFinish() to finish things up
 * else
 *   figure out selection mechanism
 *   build the correct mask value using the mode and the selection value
 *   build a list of pointers to the selected link-group structures
 *   If there are no links to process
 *     call asyncFinish() to finish things up
 *   else
 *     call processNextLink() to schecule a delay for the first link-group
 *
 *
 * NOTE:
 *   dbScanLock is already held for pR before this function is called.
 *
 *   We do NOT call processNextLink() if there is nothing to do, this prevents
 *   us from calling dbProcess() recursively.
 *
 ******************************************************************************/
static long 
process(sseqRecord *pR)
{
	struct callbackSeq	*pcb = (struct callbackSeq *) (pR->dpvt);
	struct linkDesc		*plink;
	unsigned short		lmask;
	int					tmp;

	if (sseqRecDebug > 10) {
		printf("sseqRecord: process(%s) pact = %d\n", pR->name, pR->pact);
	}

	if (pR->pact) {
		/* In async completion phase */
		asyncFinish(pR);
		return(0);
	}
	pR->pact = TRUE;

	/* Reset the PRIO in case it was changed */
	pcb->callback.priority = pR->prio;

	/* Build the selection mask */
	if (pR->selm == sseqSELM_All) {
		lmask = (unsigned short) SELN_BIT_MASK;
	} else { 
		/* Fill in the SELN field */
		if (pR->sell.type != CONSTANT) {
			dbGetLink(&(pR->sell), DBR_USHORT, &(pR->seln), 0,0);
		}
		if (pR->selm == sseqSELM_Specified) {
			if (pR->seln>10) {
				/* Invalid selection number */
				recGblSetSevr(pR,SOFT_ALARM,INVALID_ALARM);
				return(asyncFinish(pR));
			}
			if (pR->seln == 0) {
				return(asyncFinish(pR));	/* Nothing selected */
			}
			lmask = 1;
			lmask <<= pR->seln - 1;
		} else if (pR->selm == sseqSELM_Mask) {
			lmask = (pR->seln) & SELN_BIT_MASK;
		} else {
			/* Invalid selection option */
			recGblSetSevr(pR,SOFT_ALARM,INVALID_ALARM);
			return(asyncFinish(pR));
		}
	}

	/* Figure out which links are going to be processed */
	pcb->index = 0;
	plink = (struct linkDesc *)(&(pR->dly1));
	for (tmp = 1; lmask; lmask >>= 1, plink++, tmp++) {
		if (sseqRecDebug > 4) {
			printf("sseqRec:process: link %d - lnk.type=%d dol.type=%d\n",
				tmp, plink->lnk.type, plink->dol.type);
		}

		if ((lmask & 1) && ((plink->lnk.type != CONSTANT) ||
				(plink->dol.type != CONSTANT))) {
			if (sseqRecDebug > 4) {
				printf("  sseqRec:process: Adding link %d at index %d\n",
					tmp, pcb->index);
			}
			pcb->plinks[pcb->index] = plink;
			pcb->index++;
		}
	}
	pcb->plinks[pcb->index] = NULL;	/* mark the bottom of the list */

	if (!pcb->index) {
		/* There was nothing to do, finish record processing here */
		return(asyncFinish(pR));
	}

	pcb->index = 0;
	/* Start doing the first forward link (We have at least one for sure) */
	processNextLink(pR);

	return(0);
}
/*****************************************************************************
 *
 * Find the next link-group that needs processing.
 *
 * If there are no link groups left to process
 *   call bdProcess() to complete the async record processing.
 * else
 *   if the delay is > 0 seconds
 *     schedule the watch dog task to wake us up later
 *   else
 *     invoke the watch-dog wakeup routine now
 *
 *
 * NOTE:
 *   dbScanLock is already held for pR before this function is called.
 *
 ******************************************************************************/
static int processNextLink(sseqRecord *pR)
{
	struct callbackSeq	*pcb = (struct callbackSeq *) (pR->dpvt);
	struct linkDesc		*plink = (struct linkDesc *)(pcb->plinks[pcb->index]);
	int					wdDelay;

	if (sseqRecDebug > 5) {
		printf("processNextLink(%s) looking for work to do, index = %d\n",
			pR->name, pcb->index);
	}

	if (plink == NULL) {
		/* None left, finish up. */
		(*(struct rset *)(pR->rset)).process(pR);
	} else {
		if (plink->dly > 0.0) {
			/* Use the watch-dog as a delay mechanism */
			wdDelay = plink->dly * sysClkRateGet();
			wdStart(pcb->wd_id, wdDelay, (FUNCPTR)watchDog,
				(int)(&(pcb->callback)));
		} else {
			/* No delay, do it now.  Avoid recursion;  use callback task */
			watchDog(&(pcb->callback));
		}
	}
  return(0);
}
/*****************************************************************************
 *
 * Finish record processing by posting any events and processing forward links.
 *
 * NOTE:
 *   dbScanLock is already held for pR before this function is called.
 *
 ******************************************************************************/
static long
asyncFinish(sseqRecord *pR)
{
	unsigned short MonitorMask;

	if (sseqRecDebug > 5) {
		printf("asyncFinish(%s) completing processing\n", pR->name);
	}
	pR->udf = FALSE;
 
	MonitorMask = recGblResetAlarms(pR);

	if (MonitorMask) {
		db_post_events(pR, &pR->val, MonitorMask);
	}

	/* process the forward scan link record */
	recGblFwdLink(pR);

	recGblGetTimeStamp(pR);
	/* tsLocalTime(&pR->time); */
	pR->pact = FALSE;

	return(0);
}
/*****************************************************************************
 *
 * Schedule the process continuation via the callback tasks.
 *
 * This function is called by the watchdog task when it is time to process the
 * "next" link-group in the sequence record.
 *
 ******************************************************************************/
static void
watchDog(CALLBACK *pcallback)
{
	callbackRequest(pcallback);
	return;
}
/*****************************************************************************
 *
 * Link-group processing function.
 *
 * if the input link is not a constant
 *   call dbGetLink() to get the link value
 * else
 *   get the value from the DOV field
 * call dbPutLink() to forward the value to destination location
 * call processNextLink() to schedule the processing of the next link-group
 *
 * NOTE:
 *   dbScanLock is NOT held for pR when this function is called!!
 *
 ******************************************************************************/
static void
processCallback(CALLBACK *pCallback)
{
	sseqRecord			*pR = (sseqRecord *)(pCallback->user);
	struct callbackSeq	*pcb = (struct callbackSeq *) (pR->dpvt);
	struct linkDesc		*plink = (struct linkDesc *)(pcb->plinks[pcb->index]);
	double				myDouble;
	char				myString[40];
	int					status;
	char				str[40];
	double				d;

	dbScanLock((struct dbCommon *)pR);

	if (sseqRecDebug > 5) {
		printf("sseqRecord:processCallback(%s) processing field index %d\n",
			pR->name, pcb->index);
	}

	/* Save the old value */
	myDouble = plink->dov;
	strcpy(myString, plink->s);

	/* get the value */
	if (sseqRecDebug) printf("sseq:processCallback:dol_field_type=%d (%s)\n",
			plink->dol_field_type,
			pamapdbfType[plink->dol_field_type].strvalue);
	switch (plink->dol_field_type) {
	case DBF_STRING: case DBF_CHAR: case DBF_ENUM: case DBF_MENU:
	case DBF_DEVICE: case DBF_INLINK: case DBF_OUTLINK: case DBF_FWDLINK:
		status = dbGetLink(&(plink->dol), DBR_STRING, &(plink->s),0,0);
		d = atof(plink->s);
		if (d != plink->dov) {
			plink->dov = d;
			db_post_events(pR, &plink->dov, DBE_VALUE);
		}
		break;
	case DBF_UCHAR: case DBF_SHORT: case DBF_USHORT: case DBF_LONG:
	case DBF_ULONG: case DBF_FLOAT: case DBF_DOUBLE:
		status = dbGetLink(&(plink->dol), DBR_DOUBLE, &(plink->dov),0,0);
		cvtDoubleToString(plink->dov, str, pR->prec);
		if (strcmp(str, plink->s)) {
			strcpy(plink->s, str);
			db_post_events(pR, &plink->s, DBE_VALUE);
		}
		break;
	default:
		break;
	}

	/* Dump the value to the destination field */
	if (sseqRecDebug) printf("sseq:processCallback:lnk_field_type=%d (%s)\n",
			plink->lnk_field_type,
			pamapdbfType[plink->lnk_field_type].strvalue);
	switch (plink->lnk_field_type) {
	case DBF_STRING: case DBF_CHAR: case DBF_ENUM: case DBF_MENU:
	case DBF_DEVICE: case DBF_INLINK: case DBF_OUTLINK: case DBF_FWDLINK:
		status = dbPutLink(&(plink->lnk), DBR_STRING, &(plink->s),1);
		break;
	case DBF_UCHAR: case DBF_SHORT: case DBF_USHORT: case DBF_LONG:
	case DBF_ULONG: case DBF_FLOAT: case DBF_DOUBLE:
		status = dbPutLink(&(plink->lnk), DBR_DOUBLE, &(plink->dov),1);
		break;
	default:
		break;
	}

	if (myDouble != plink->dov) {
		if (sseqRecDebug > 0) {
			printf("link %d changed from %f to %f\n", pcb->index, myDouble,
				plink->dov);
		}
		db_post_events(pR, &plink->dov, DBE_VALUE|DBE_LOG);
	} else if (strcmp(myString, plink->s)) {
		if (sseqRecDebug > 0) {
			printf("link %d changed from '%s' to '%s'\n", pcb->index, myString,
				plink->s);
		}
		db_post_events(pR, &plink->s, DBE_VALUE|DBE_LOG);
	}

	/* Find the 'next' link-seq that is ready for processing. */
	pcb->index++;
	processNextLink(pR);

	dbScanUnlock((struct dbCommon *)pR);
	return;
}
/*****************************************************************************
 *
 * Return the precision value from PREC
 *
 *****************************************************************************/
static long
get_precision(struct dbAddr *paddr, long *precision)
{
	sseqRecord	*pR = (struct sseqRecord *) paddr->precord;

	*precision = pR->prec;

	if (paddr->pfield < (void *)&pR->val) {
		return(0);						/* Field is NOT in dbCommon */
	}

	recGblGetPrec(paddr, precision);	/* Field is in dbCommon */
	return(0);
}


static void checkLinksCallback(CALLBACK *pCallback)
{
    sseqRecord			*pR;
	struct callbackSeq	*pdpvt;

    callbackGetUser(pR, pCallback);
    pdpvt = (struct callbackSeq	*)pR->dpvt;
    
	if (!interruptAccept) {
		/* Can't call dbScanLock yet.  Schedule another CALLBACK */
		pdpvt->wd_id_1_LOCK = 1;  /* make sure */
		wdStart(pdpvt->wd_id_1, 30, (FUNCPTR)callbackRequest,
			(int)(&pdpvt->checkLinksCB));
	} else {
	    dbScanLock((struct dbCommon *)pR);
	    pdpvt->wd_id_1_LOCK = 0;
	    checkLinks(pR);
	    dbScanUnlock((struct dbCommon *)pR);
	}
}


static void checkLinks(sseqRecord *pR)
{
	struct linkDesc *plink = (struct linkDesc *)(&(pR->dly1));
	struct callbackSeq	*pdpvt = (struct callbackSeq *)pR->dpvt;
	int i;

	if (sseqRecDebug) printf("sseq:checkLinks(%s)\n", pR->name);

	pdpvt->linkStat = LINKS_ALL_OK;
	for (i = 0; i < NUM_LINKS; i++, plink++) {
		plink->dol_field_type = DBF_unknown;
		if (plink->dol.value.pv_link.pvname[0]) {
			plink->dol_field_type = dbGetLinkDBFtype(&plink->dol);
			if (plink->dol_field_type < 0) pdpvt->linkStat = LINKS_NOT_OK;
			if (sseqRecDebug) printf("sseq:checkLinks:dol_field_type=%d (%s)\n",
				plink->dol_field_type,
				pamapdbfType[plink->dol_field_type].strvalue);
		}
		plink->lnk_field_type = DBF_unknown;
		if (plink->lnk.value.pv_link.pvname[0]) {
			plink->lnk_field_type = dbGetLinkDBFtype(&plink->lnk);
			if (plink->lnk_field_type < 0) pdpvt->linkStat = LINKS_NOT_OK;
			if (sseqRecDebug) printf("sseq:checkLinks:lnk_field_type=%d (%s)\n",
				plink->lnk_field_type,
				pamapdbfType[plink->lnk_field_type].strvalue);
		}
	}
	if (!pdpvt->wd_id_1_LOCK && (pdpvt->linkStat == LINKS_NOT_OK)) {
		/* Schedule another CALLBACK */
		pdpvt->wd_id_1_LOCK = 1;
		wdStart(pdpvt->wd_id_1, 30, (FUNCPTR)callbackRequest,
			(int)(&pdpvt->checkLinksCB));
	}
}


static long special(struct dbAddr *paddr, int after)
{
	sseqRecord			*pR = (sseqRecord *)(paddr->precord);
	struct callbackSeq	*pdpvt = (struct callbackSeq *)pR->dpvt;
	int                 fieldIndex = dbGetFieldIndex(paddr);
	int                 lnkIndex;
	struct linkDesc		*plink;
	char				str[40];
	double				d;

	if (sseqRecDebug) printf("sseq:special(%s)\n", pR->name);
	if (!after) return(0);
	switch (fieldIndex) {
	case(sseqRecordDOL1):
	case(sseqRecordDOL2):
	case(sseqRecordDOL3):
	case(sseqRecordDOL4):
	case(sseqRecordDOL5):
	case(sseqRecordDOL6):
	case(sseqRecordDOL7):
	case(sseqRecordDOL8):
	case(sseqRecordDOL9):
	case(sseqRecordDOLA):
		lnkIndex = ((char *)paddr->pfield - (char *)&pR->dly1) /
			sizeof(struct linkDesc);
		plink = (struct linkDesc *)&pR->dly1;
		plink += lnkIndex;
		plink->dol_field_type = DBF_unknown;
		if (plink->dol.value.pv_link.pvname[0]) {
			plink->dol_field_type = dbGetLinkDBFtype(&plink->dol);
			if (plink->dol_field_type < 0) pdpvt->linkStat = LINKS_NOT_OK;
		}
		if (!pdpvt->wd_id_1_LOCK && (pdpvt->linkStat == LINKS_NOT_OK)) {
			pdpvt->wd_id_1_LOCK = 1;
			wdStart(pdpvt->wd_id_1, 30, (FUNCPTR)callbackRequest,
				(int)(&pdpvt->checkLinksCB));
		}
		if (sseqRecDebug) printf("sseq:special:dol_field_type=%d (%s)\n",
			plink->dol_field_type,
			pamapdbfType[plink->dol_field_type].strvalue);
		return(0);
		break;

	case(sseqRecordLNK1):
	case(sseqRecordLNK2):
	case(sseqRecordLNK3):
	case(sseqRecordLNK4):
	case(sseqRecordLNK5):
	case(sseqRecordLNK6):
	case(sseqRecordLNK7):
	case(sseqRecordLNK8):
	case(sseqRecordLNK9):
	case(sseqRecordLNKA):
		lnkIndex = ((char *)paddr->pfield - (char *)&pR->dly1) /
			sizeof(struct linkDesc);
		plink = (struct linkDesc *)&pR->dly1;
		plink += lnkIndex;
		if (sseqRecDebug) {
			printf("sseq:special:lnkIndex=%d\n", lnkIndex);
			printf("sseq:special: &lnk1=%p, &plink->lnk=%p\n",
				&pR->lnk1, &plink->lnk);
		}
		plink->lnk_field_type = DBF_unknown;
		if (plink->lnk.value.pv_link.pvname[0]) {
			plink->lnk_field_type = dbGetLinkDBFtype(&plink->lnk);
			if (plink->lnk_field_type < 0) pdpvt->linkStat = LINKS_NOT_OK;
		}
		if (!pdpvt->wd_id_1_LOCK && (pdpvt->linkStat == LINKS_NOT_OK)) {
			pdpvt->wd_id_1_LOCK = 1;
			wdStart(pdpvt->wd_id_1, 30, (FUNCPTR)callbackRequest,
				(int)(&pdpvt->checkLinksCB));
		}
		if (sseqRecDebug) printf("sseq:special:lnk_field_type=%d (%s)\n",
			plink->lnk_field_type,
			pamapdbfType[plink->lnk_field_type].strvalue);
		return(0);
		break;

	case(sseqRecordDO1):
	case(sseqRecordDO2):
	case(sseqRecordDO3):
	case(sseqRecordDO4):
	case(sseqRecordDO5):
	case(sseqRecordDO6):
	case(sseqRecordDO7):
	case(sseqRecordDO8):
	case(sseqRecordDO9):
	case(sseqRecordDOA):
		lnkIndex = ((char *)paddr->pfield - (char *)&pR->dly1) /
			sizeof(struct linkDesc);
		plink = (struct linkDesc *)&pR->dly1;
		plink += lnkIndex;
		cvtDoubleToString(plink->dov, str, pR->prec);
		if (strcmp(str, plink->s)) {
			strcpy(plink->s, str);
			db_post_events(pR, &plink->s, DBE_VALUE);
		}
		break;

	case(sseqRecordSTR1):
	case(sseqRecordSTR2):
	case(sseqRecordSTR3):
	case(sseqRecordSTR4):
	case(sseqRecordSTR5):
	case(sseqRecordSTR6):
	case(sseqRecordSTR7):
	case(sseqRecordSTR8):
	case(sseqRecordSTR9):
	case(sseqRecordSTRA):
		lnkIndex = ((char *)paddr->pfield - (char *)&pR->dly1) /
			sizeof(struct linkDesc);
		plink = (struct linkDesc *)&pR->dly1;
		plink += lnkIndex;
		d = atof(plink->s);
		if (d != plink->dov) {
			plink->dov = d;
			db_post_events(pR, &plink->dov, DBE_VALUE);
		}
		break;

	default:
		recGblDbaddrError(S_db_badChoice,paddr,"sseq: special");
		return(S_db_badChoice);
	}
	return(0);
}