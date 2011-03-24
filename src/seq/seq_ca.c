/*	Process Variable interface for sequencer.
 *
 *	Author:  Andy Kozubal
 *	Date:    July, 1991
 *
 *	Experimental Physics and Industrial Control System (EPICS)
 *
 *	Copyright 1991-1994, the Regents of the University of California,
 *	and the University of Chicago Board of Governors.
 *
 *	Copyright, 2010, Helmholtz-Zentrum Berlin f. Materialien
 *		und Energie GmbH, Germany (HZB)
 *		(see file Copyright.HZB included in this distribution)
 *
 *	This software was produced under  U.S. Government contracts:
 *	(W-7405-ENG-36) at the Los Alamos National Laboratory,
 *	and (W-31-109-ENG-38) at Argonne National Laboratory.
 *
 *	Initial development by:
 *	  The Controls and Automation Group (AT-8)
 *	  Ground Test Accelerator
 *	  Accelerator Technology Division
 *	  Los Alamos National Laboratory
 *
 *	Co-developed with
 *	  The Controls and Computing Group
 *	  Accelerator Systems Division
 *	  Advanced Photon Source
 *	  Argonne National Laboratory
 */
#include "seq.h"

/* #define DEBUG errlogPrintf */
#define DEBUG nothing

/*
 * Event type (extra argument passed to proc_db_events().
 */
enum event_type {
	GET_COMPLETE,
	PUT_COMPLETE,
	MON_COMPLETE
};

static const char *event_type_name[] = {
	"get",
	"put",
	"mon"
};

static void proc_db_events(pvValue *value, pvType type,
	CHAN *ch, SSCB *ss, enum event_type evtype, pvStat status);
static void proc_db_events_queued(SPROG *sp, CHAN *ch, pvValue *value);

/*
 * seq_connect() - Initiate connect & monitor requests to PVs.
 * If wait is TRUE, wait for all connections to be established.
 */
pvStat seq_connect(SPROG *sp, boolean wait)
{
	pvStat		status;
	unsigned	nch;
	int		delay = 10;
	boolean		ready = FALSE;

	for (nch = 0; nch < sp->numChans; nch++)
	{
		CHAN *ch = sp->chan + nch;

		if (ch->ach == NULL)
			continue;		/* skip anonymous pvs */
		sp->assignCount += 1;
		if (ch->monitored)
			sp->monitorCount++;	/* do it before pvVarCreate */
	}

	/*
	 * For each channel: create pv object, then subscribe if monitored.
	 */
	for (nch = 0; nch < sp->numChans; nch++)
	{
		CHAN *ch = sp->chan + nch;
		ACHAN *ach = ch->ach;

		if (ach == NULL)
			continue; /* skip records without pv names */
		DEBUG("seq_connect: connect %s to %s\n", ch->varName,
			ach->dbName);
		/* Connect to it */
		status = pvVarCreate(
				sp->pvSys,		/* PV system context */
				ch->ach->dbName,	/* PV name */
				seq_conn_handler,	/* connection handler routine */
				ch,			/* private data is CHAN struc */
				0,			/* debug level (inherited) */
				&ach->pvid);		/* ptr to PV id */
		if (status != pvStatOK)
		{
			errlogPrintf("seq_connect: pvVarCreate() %s failure: "
				"%s\n", ach->dbName, pvVarGetMess(ach->pvid));
			continue;
		}
	}
	pvSysFlush(sp->pvSys);

	if (wait)
	{
		do {
			/* Check whether we have been asked to exit */
			if (sp->die)
				return pvStatERROR;

			epicsMutexMustLock(sp->programLock);
			ready = sp->connectCount == sp->assignCount;
			epicsMutexUnlock(sp->programLock);

			if (!ready) {
				epicsEventWaitStatus status = epicsEventWaitWithTimeout(
					sp->ready, (double)delay);
				if (status == epicsEventWaitError || sp->die)
					return pvStatERROR;
				if (delay < 60) delay += 10;
				errlogPrintf("%s[%d]: assignCount=%d, connectCount=%d, monitorCount=%d\n",
					sp->progName, sp->instance,
					sp->assignCount, sp->connectCount, sp->monitorCount);
			}
		} while (!ready);
	}
	return pvStatOK;
}

/*
 * seq_get_handler() - Sequencer callback handler.
 * Called when a "get" completes.
 */
epicsShareFunc void seq_get_handler(
	void *var, pvType type, int count, pvValue *value, void *arg, pvStat status)
{
	PVREQ	*rQ = (PVREQ *)arg;
	CHAN	*ch = rQ->ch;
	SPROG	*sp = ch->sprog;

	assert(ch->ach != NULL);
	freeListFree(sp->pvReqPool, arg);
	/* Process event handling in each state set */
	proc_db_events(value, type, ch, rQ->ss, GET_COMPLETE, status);
}

/*
 * seq_put_handler() - Sequencer callback handler.
 * Called when a "put" completes.
 */
epicsShareFunc void seq_put_handler(
	void *var, pvType type, int count, pvValue *value, void *arg, pvStat status)
{
	PVREQ	*rQ = (PVREQ *)arg;
	CHAN	*ch = rQ->ch;
	SPROG	*sp = ch->sprog;

	assert(ch->ach != NULL);
	freeListFree(sp->pvReqPool, arg);
	/* Process event handling in each state set */
	proc_db_events(value, type, ch, rQ->ss, PUT_COMPLETE, status);
}

/*
 * seq_mon_handler() - PV events (monitors) come here.
 */
epicsShareFunc void seq_mon_handler(
	void *var, pvType type, int count, pvValue *value, void *arg, pvStat status)
{
	CHAN	*ch = (CHAN *)arg;
	SPROG	*sp = ch->sprog;
	ACHAN	*ach = ch->ach;

	assert(ach != NULL);
	/* Process event handling in each state set */
	proc_db_events(value, type, ch, sp->ss, MON_COMPLETE, status);
	if (!ach->gotOneMonitor)
	{
		ach->gotOneMonitor = TRUE;
		epicsMutexMustLock(sp->programLock);
		sp->firstMonitorCount++;
		if (sp->firstMonitorCount == sp->monitorCount
			&& sp->connectCount == sp->assignCount)
		{
			epicsEventSignal(sp->ready);
		}
		epicsMutexUnlock(sp->programLock);
	}
}

/* Common code for completion and monitor handling */
static void proc_db_events(
	pvValue	*value,
	pvType	type,
	CHAN	*ch,
	SSCB	*ss,		/* originator, for put and get */
	enum event_type evtype,
	pvStat	status)
{
	SPROG	*sp = ch->sprog;
	ACHAN	*ach = ch->ach;

	assert(ach != NULL);
	DEBUG("proc_db_events: var=%s, pv=%s, type=%s, status=%d\n", ch->varName,
		ach->dbName, event_type_name[evtype], status);

	/* If monitor on var queued via syncQ, branch to alternative routine */
	if (ch->queue && evtype == MON_COMPLETE)
	{
		proc_db_events_queued(sp, ch, value);
		return;
	}

	/* Copy value returned into user variable (can get NULL value pointer
	   for put completion only) */
	if (value != NULL)
	{
		void *val = pv_value_ptr(value,type);
                PVMETA *meta = metaPtr(ch,ss);

		/* Write value to CA buffer (lock-free) */
		ss_write_buffer(0, ch, val);

		/* Copy status, severity and time stamp */
		meta->status = *pv_status_ptr(value,type);
		meta->severity = *pv_severity_ptr(value,type);
		meta->timeStamp = *pv_stamp_ptr(value,type);

		/* Copy error message (only when severity indicates error) */
		if (meta->severity != pvSevrNONE)
		{
			const char *pmsg = pvVarGetMess(ach->pvid);
			if (!pmsg) pmsg = "unknown";
			meta->message = pmsg;
		}
	}

	/* Wake up each state set that uses this channel in an event */
	seqWakeup(sp, ch->eventNum);

	/* If there's an event flag associated with this channel, set it */
	/* TODO: check if correct/documented to do this for non-monitor completions */
	if (ch->efId > 0)
		seq_efSet(sp->ss, ch->efId);

	/* Signal completion */
	switch (evtype)
	{
	    case GET_COMPLETE:
		epicsEventSignal(ss->getSemId[chNum(ch)]);
		break;
	    case PUT_COMPLETE:
		epicsEventSignal(ss->putSemId[chNum(ch)]);
		break;
	    default:
		break;
	}
}

/* Common code for event and callback handling (queuing version) */
static void proc_db_events_queued(SPROG *sp, CHAN *ch, pvValue *value)
{
	boolean	full;

	DEBUG("proc_db_events_queued: var=%s, pv=%s, queue=%p, used(max)=%d(%d)\n",
		ch->varName, ch->ach->dbName,
		ch->queue, seqQueueUsed(ch->queue), seqQueueNumElems(ch->queue));
	/* Copy whole message into queue; no lock needed because only one writer */
	full = seqQueuePut(ch->queue, value);
	if (full)
	{
		errlogSevPrintf(errlogMinor,
		  "monitor event for variable %s (pv %s): "
		  "last queue element overwritten (queue is full)\n",
		  ch->varName, ch->ach->dbName
		);
	}
	/* Set event flag; note: it doesn't matter which state set we pass. */
	seq_efSet(sp->ss, ch->efId);
	/* Wake up each state set that uses this channel in an event */
	seqWakeup(sp, ch->eventNum);
}

/* Disconnect all database channels */
epicsShareFunc void seq_disconnect(SPROG *sp)
{
	unsigned nch;

	DEBUG("seq_disconnect: sp = %p\n", sp);

	epicsMutexMustLock(sp->programLock);
	for (nch = 0; nch < sp->numChans; nch++)
	{
		CHAN	*ch = sp->chan + nch;
		pvStat	status;
		ACHAN	*ach = ch->ach;

		if (!ach)
			continue;
		DEBUG("seq_disconnect: disconnect %s from %s\n",
 			ch->varName, ach->dbName);
		/* Disconnect this PV */
		status = pvVarDestroy(ach->pvid);
		if (status != pvStatOK)
		    errlogPrintf("seq_disconnect: pvVarDestroy() %s failure: "
				"%s\n", ach->dbName, pvVarGetMess(ach->pvid));

		/* Clear monitor & connect indicators */
		ach->connected = FALSE;
		sp->connectCount -= 1;
	}
	epicsMutexUnlock(sp->programLock);

	pvSysFlush(sp->pvSys);
}

pvStat seq_monitor(CHAN *ch, boolean on)
{
	ACHAN	*ach = ch->ach;
	pvStat	status;

	assert(ch);
	assert(ach);
	if (on == (ach->monid != NULL))			/* already done */
		return pvStatOK;
	DEBUG("calling pvVarMonitor%s(%p)\n", on?"On":"Off", ach->pvid);
	if (on)
		status = pvVarMonitorOn(
				ach->pvid,		/* pvid */
				ch->type->getType,	/* requested type */
				(int)ch->count,		/* element count */
				seq_mon_handler,	/* function to call */
				ch,			/* user arg (channel struct) */
				&ach->monid);		/* where to put event id */
	else
		status = pvVarMonitorOff(ach->pvid, ach->monid);
	if (status != pvStatOK)
		errlogPrintf("seq_monitor: pvVarMonitor%s(%s) failure: %s\n",
			on?"On":"Off", ach->dbName, pvVarGetMess(ach->pvid));
	else if (!on)
		ach->monid = NULL;
	return status;
}

/*
 * seq_conn_handler() - Sequencer connection handler.
 * Called each time a connection is established or broken.
 */
void seq_conn_handler(void *var, int connected)
{
	CHAN	*ch = (CHAN *)pvVarGetPrivate(var);
	SPROG	*sp = ch->sprog;
	ACHAN	*ach = ch->ach;

	epicsMutexMustLock(sp->programLock);

	assert(ach != NULL);

	/* Note that CA may call this while pvVarCreate is still running,
	   so ach->pvid may not yet be initialized. Since the call
	   to seq_monitor below uses it we have to prepone initialization
	   at this point.
	  */
	if (!ach->pvid)
		ach->pvid = var;

	assert(ach->pvid == var);
	if (!connected)
	{
		DEBUG("%s disconnected from %s\n", ch->varName, ach->dbName);
		if (ach->connected)
		{
			ach->connected = FALSE;
			sp->connectCount--;

			if (ch->monitored)
			{
				seq_monitor(ch, FALSE);
			}
		}
		else
		{
			errlogPrintf("%s disconnect event but already disconnected %s\n",
				ch->varName, ach->dbName);
		}
	}
	else	/* connected */
	{
		DEBUG("%s connected to %s\n", ch->varName, ach->dbName);
		if (!ach->connected)
		{
			int dbCount;
			ach->connected = TRUE;
			sp->connectCount++;
			if (sp->firstMonitorCount == sp->monitorCount
				&& sp->connectCount == sp->assignCount)
			{
				epicsEventSignal(sp->ready);
			}
			dbCount = pvVarGetCount(var);
			assert(dbCount >= 0);
			ach->dbCount = min(ch->count, (unsigned)dbCount);

			if (ch->monitored)
			{
				seq_monitor(ch, TRUE);
			}
		}
		else
		{
			errlogPrintf("%s connect event but already connected %s\n",
				ch->varName, ach->dbName);
		}
	}
	epicsMutexUnlock(sp->programLock);

	/* Wake up each state set that is waiting for event processing.
	   Why each one? Because pvConnectCount and pvMonitorCount should
	   act like monitored anonymous channels. Any state set might be
	   using these functions inside a when-condition and it is expected
	   that such conditions get checked whenever these counts change.
	   This is really a crude solution. */
	seqWakeup(sp, 0);
}
