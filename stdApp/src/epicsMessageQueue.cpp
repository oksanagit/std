/*************************************************************************\
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE Versions 3.13.7
* and higher are distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution. 
\*************************************************************************/
/*
 *      $Id: epicsMessageQueue.cpp,v 1.1 2003-05-28 21:17:51 bcda Exp $
 *
 *      Author  W. Eric Norum
 *              norume@aps.anl.gov
 *              630 252 4793
 */

#include <new>

#define epicsExportSharedSymbols
#include "epicsMessageQueue.h"

epicsMessageQueue::epicsMessageQueue(unsigned int aCapacity,
                                     unsigned int aMaxMessageSize) 
    : id ( epicsMessageQueueCreate(aCapacity, aMaxMessageSize) )
{
    if (id == NULL)
        throw std::bad_alloc ();
}

epicsMessageQueue::~epicsMessageQueue()
{
    epicsMessageQueueDestroy(id);
}

int
epicsMessageQueue::trySend(void *message, unsigned int size)
{
    return epicsMessageQueueTrySend(id, message, size);
}

int
epicsMessageQueue::send(void *message, unsigned int size)
{
    return epicsMessageQueueSend(id, message, size);
}

int
epicsMessageQueue::send(void *message, unsigned int size, double timeout)
{
    return epicsMessageQueueSendWithTimeout(id, message, size, timeout);
}

int
epicsMessageQueue::tryReceive(void *message )
{
    return epicsMessageQueueTryReceive(id, message);
}

int
epicsMessageQueue::receive(void *message )
{
    return epicsMessageQueueReceive(id, message);
}

int
epicsMessageQueue::receive(void *message, double timeout)
{
    return epicsMessageQueueReceiveWithTimeout(id, message, timeout);
}

unsigned int
epicsMessageQueue::pending()
{
    return epicsMessageQueuePending(id);
}

void
epicsMessageQueue::show(unsigned int level)
{
    epicsMessageQueueShow(id, level);
}