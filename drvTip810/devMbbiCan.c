/*******************************************************************************

Project:
    Gemini/UKIRT CAN Bus Driver for EPICS

File:
    devMbbiCan.c

Description:
    CANBUS Multi-Bit Binary Input device support

Author:
    Andrew Johnson
Created:
    14 August 1995
Version:
    $Id: devMbbiCan.c,v 1.2 1997-06-19 16:57:18 anj Exp $

(c) 1995 Royal Greenwich Observatory

*******************************************************************************/


#include <vxWorks.h>
#include <stdlib.h>
#include <wdLib.h>

#include <errMdef.h>
#include <devLib.h>
#include <dbAccess.h>
#include <dbScan.h>
#include <callback.h>
#include <cvtTable.h>
#include <link.h>
#include <recSup.h>
#include <devSup.h>
#include <dbCommon.h>
#include <mbbiRecord.h>
#include <canBus.h>


#define CONVERT 0
#define DO_NOT_CONVERT 2


typedef struct mbbiCanPrivate_s {
    CALLBACK callback;		/* This *must* be first member */
    struct mbbiCanPrivate_s *nextPrivate;
    WDOG_ID wdId;
    IOSCANPVT ioscanpvt;
    struct mbbiRecord *prec;
    canIo_t inp;
    long data;
    int status;
} mbbiCanPrivate_t;

typedef struct mbbiCanBus_s {
    CALLBACK callback;		/* This *must* be first member */
    struct mbbiCanBus_s *nextBus;
    mbbiCanPrivate_t *firstPrivate;
    void *canBusID;
    int status;
} mbbiCanBus_t;

LOCAL long init_mbbi(struct mbbiRecord *prec);
LOCAL long get_ioint_info(int cmd, struct mbbiRecord *prec, IOSCANPVT *ppvt);
LOCAL long read_mbbi(struct mbbiRecord *prec);
LOCAL void mbbiProcess(mbbiCanPrivate_t *pcanMbbi);
LOCAL void mbbiMessage(mbbiCanPrivate_t *pcanMbbi, canMessage_t *pmessage);
LOCAL void busSignal(mbbiCanBus_t *pbus, int status);
LOCAL void busCallback(mbbiCanBus_t *pbus);

struct {
    long number;
    DEVSUPFUN report;
    DEVSUPFUN init;
    DEVSUPFUN init_record;
    DEVSUPFUN get_ioint_info;
    DEVSUPFUN read_mbbi;
} devMbbiCan = {
    5,
    NULL,
    NULL,
    init_mbbi,
    get_ioint_info,
    read_mbbi
};

LOCAL mbbiCanBus_t *firstBus;


LOCAL long init_mbbi (
    struct mbbiRecord *prec
) {
    mbbiCanPrivate_t *pcanMbbi;
    mbbiCanBus_t *pbus;
    int status;
    
    if (prec->inp.type != INST_IO) {
	recGblRecordError(S_db_badField, (void *) prec,
			  "devMbbiCan (init_record) Illegal INP field");
	return S_db_badField;
    }

    pcanMbbi = (mbbiCanPrivate_t *) malloc(sizeof(mbbiCanPrivate_t));
    if (pcanMbbi == NULL) {
	return S_dev_noMemory;
    }
    prec->dpvt = pcanMbbi;
    pcanMbbi->prec = prec;
    pcanMbbi->ioscanpvt = NULL;
    pcanMbbi->status = NO_ALARM;

    /* Convert the address string into members of the canIo structure */
    status = canIoParse(prec->inp.value.instio.string, &pcanMbbi->inp);
    if (status ||
	pcanMbbi->inp.parameter < 0 ||
	pcanMbbi->inp.parameter > 7) {
	if (canSilenceErrors) {
	    pcanMbbi->inp.canBusID = NULL;
	    prec->pact = TRUE;
	    return OK;
	} else {
	    recGblRecordError(S_can_badAddress, (void *) prec,
			      "devMbbiCan (init_record) bad CAN address");
	    return S_can_badAddress;
	}
    }

    #ifdef DEBUG
	printf("mbbiCan %s: Init bus=%s, id=%#x, off=%d, parm=%d\n",
		    prec->name, pcanMbbi->inp.busName, pcanMbbi->inp.identifier,
		    pcanMbbi->inp.offset, pcanMbbi->inp.parameter);
    #endif

    /* For mbbi records, the final parameter specifies the input bit shift,
       with offset specifying the message byte number. */
    prec->shft = pcanMbbi->inp.parameter;
    prec->mask <<= pcanMbbi->inp.parameter;

    #ifdef DEBUG
	printf("  shft=%d, mask=%#x\n", 
		pcanMbbi->inp.parameter, prec->mask);
    #endif

    /* Find the bus matching this record */
    for (pbus = firstBus; pbus != NULL; pbus = pbus->nextBus) {
      if (pbus->canBusID == pcanMbbi->inp.canBusID) break;
    }  

    /* If not found, create one */
    if (pbus == NULL) {
      pbus = malloc(sizeof (mbbiCanBus_t));
      if (pbus == NULL) return S_dev_noMemory;

      /* Fill it in */
      pbus->firstPrivate = NULL;
      pbus->canBusID = pcanMbbi->inp.canBusID;
      callbackSetCallback(busCallback, &pbus->callback);
      callbackSetPriority(priorityMedium, &pbus->callback);

      /* and add it to the list of busses we know about */
      pbus->nextBus = firstBus;
      firstBus = pbus;

      /* Ask driver for error signals */
      canSignal(pbus->canBusID, (canSigCallback_t *) busSignal, pbus);
    }  

    /* Insert private record structure into linked list for this CANbus */
    pcanMbbi->nextPrivate = pbus->firstPrivate;
    pbus->firstPrivate = pcanMbbi;

    /* Set the callback parameters for asynchronous processing */
    callbackSetCallback(mbbiProcess, &pcanMbbi->callback);
    callbackSetPriority(prec->prio, &pcanMbbi->callback);

    /* and create a watchdog for CANbus RTR timeouts */
    pcanMbbi->wdId = wdCreate();
    if (pcanMbbi->wdId == NULL) {
	return S_dev_noMemory;
    }

    /* Register the message handler with the Canbus driver */
    canMessage(pcanMbbi->inp.canBusID, pcanMbbi->inp.identifier, 
	       (canMsgCallback_t *) mbbiMessage, pcanMbbi);

    return OK;
}

LOCAL long get_ioint_info (
    int cmd,
    struct mbbiRecord *prec, 
    IOSCANPVT *ppvt
) {
    mbbiCanPrivate_t *pcanMbbi = (mbbiCanPrivate_t *) prec->dpvt;

    if (pcanMbbi->ioscanpvt == NULL) {
	scanIoInit(&pcanMbbi->ioscanpvt);
    }

    #ifdef DEBUG
	printf("canMbbi %s: get_ioint_info %d\n", prec->name, cmd);
    #endif

    *ppvt = pcanMbbi->ioscanpvt;
    return OK;
}

LOCAL long read_mbbi (
    struct mbbiRecord *prec
) {
    mbbiCanPrivate_t *pcanMbbi = (mbbiCanPrivate_t *) prec->dpvt;

    if (pcanMbbi->inp.canBusID == NULL) {
	return DO_NOT_CONVERT;
    }

    #ifdef DEBUG
	printf("canMbbi %s: read_mbbi status=%#x\n", prec->name, pcanMbbi->status);
    #endif

    switch (pcanMbbi->status) {
	case TIMEOUT_ALARM:
	case COMM_ALARM:
	    recGblSetSevr(prec, pcanMbbi->status, MAJOR_ALARM);
	    pcanMbbi->status = NO_ALARM;
	    return DO_NOT_CONVERT;
	case READ_ALARM:
	    recGblSetSevr(prec, COMM_ALARM, MINOR_ALARM);
	    pcanMbbi->status = NO_ALARM;
	    return DO_NOT_CONVERT;

	case NO_ALARM:
	    if (prec->pact || prec->scan == SCAN_IO_EVENT) {
		#ifdef DEBUG
		    printf("canMbbi %s: message id=%#x, data=%#x\n", 
			    prec->name, pcanMbbi->inp.identifier, pcanMbbi->data);
		#endif

		prec->rval = pcanMbbi->data & prec->mask;
		return CONVERT;
	    } else {
		canMessage_t message;

		message.identifier = pcanMbbi->inp.identifier;
		message.rtr = RTR;
		message.length = 0;

		#ifdef DEBUG
		    printf("canMbbi %s: RTR, id=%#x\n", 
			    prec->name, pcanMbbi->inp.identifier);
		#endif

		prec->pact = TRUE;
		pcanMbbi->status = TIMEOUT_ALARM;

		callbackSetPriority(prec->prio, &pcanMbbi->callback);
		wdStart(pcanMbbi->wdId, pcanMbbi->inp.timeout, 
			(FUNCPTR) callbackRequest, (int) pcanMbbi);
		canWrite(pcanMbbi->inp.canBusID, &message, pcanMbbi->inp.timeout);
		return 0;
	    }
	default:
	    recGblSetSevr(prec, UDF_ALARM, INVALID_ALARM);
	    pcanMbbi->status = NO_ALARM;
	    return DO_NOT_CONVERT;
    }
}

LOCAL void mbbiProcess (
    mbbiCanPrivate_t *pcanMbbi
) {
    dbScanLock((struct dbCommon *) pcanMbbi->prec);
    (*((struct rset *) pcanMbbi->prec->rset)->process)(pcanMbbi->prec);
    dbScanUnlock((struct dbCommon *) pcanMbbi->prec);
}

LOCAL void mbbiMessage (
    mbbiCanPrivate_t *pcanMbbi,
    canMessage_t *pmessage
) {
    if (!interruptAccept) return;
    
    if (pmessage->rtr == RTR) {
	return;		/* Ignore RTRs */
    }

    pcanMbbi->data = pmessage->data[pcanMbbi->inp.offset];

    if (pcanMbbi->prec->scan == SCAN_IO_EVENT) {
	pcanMbbi->status = NO_ALARM;
	scanIoRequest(pcanMbbi->ioscanpvt);
    } else if (pcanMbbi->status == TIMEOUT_ALARM) {
	pcanMbbi->status = NO_ALARM;
	wdCancel(pcanMbbi->wdId);
	callbackRequest(&pcanMbbi->callback);
    }
}

LOCAL void busSignal (
    mbbiCanBus_t *pbus,
    int status
) {
    if (!interruptAccept) return;
    
    switch(status) {
	case CAN_BUS_OK:
	    pbus->status = NO_ALARM;
	    return;
	case CAN_BUS_ERROR:
	    pbus->status = READ_ALARM;
	    break;
	case CAN_BUS_OFF:
	    pbus->status = COMM_ALARM;
	    break;
    }
    callbackRequest(&pbus->callback);
}

LOCAL void busCallback (
    mbbiCanBus_t *pbus
) {
    mbbiCanPrivate_t *pcanMbbi = pbus->firstPrivate;
    
    while (pcanMbbi != NULL) {
	pcanMbbi->status = pbus->status;
	mbbiProcess(pcanMbbi);
	pcanMbbi = pcanMbbi->nextPrivate;
    }
}
