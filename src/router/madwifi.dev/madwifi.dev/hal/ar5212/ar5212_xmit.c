/*
 * Copyright (c) 2002-2006 Sam Leffler, Errno Consulting
 * Copyright (c) 2002-2006 Atheros Communications, Inc.
 * All rights reserved.
 *
 * $Id: //depot/sw/branches/sam_hal/ar5212/ar5212_xmit.c#7 $
 */
#include "opt_ah.h"

#ifdef AH_SUPPORT_AR5212

#include "ah.h"
#include "ah_xr.h"
#include "ah_internal.h"

#include "ar5212/ar5212.h"
#include "ar5212/ar5212reg.h"
#include "ar5212/ar5212desc.h"
#include "ar5212/ar5212phy.h"
#ifdef AH_SUPPORT_5311
#include "ar5212/ar5311reg.h"
#endif

#ifdef AH_NEED_DESC_SWAP
static void ar5212SwapTxDesc(struct ath_desc *ds);
#endif

/*
 * Update Tx FIFO trigger level.
 *
 * Set bIncTrigLevel to TRUE to increase the trigger level.
 * Set bIncTrigLevel to FALSE to decrease the trigger level.
 *
 * Returns TRUE if the trigger level was updated
 */
HAL_BOOL
ar5212UpdateTxTrigLevel(struct ath_hal *ah, HAL_BOOL bIncTrigLevel)
{
	struct ath_hal_5212 *ahp = AH5212(ah);
	u_int32_t txcfg, curLevel, newLevel;
	HAL_INT omask;

	/*
	 * Disable interrupts while futzing with the fifo level.
	 */
	omask = ar5212SetInterrupts(ah, ahp->ah_maskReg &~ HAL_INT_GLOBAL);

	txcfg = OS_REG_READ(ah, AR_TXCFG);
	curLevel = MS(txcfg, AR_FTRIG);
	newLevel = curLevel;
	if (bIncTrigLevel) {		/* increase the trigger level */
		if (curLevel < MAX_TX_FIFO_THRESHOLD)
			newLevel++;
	} else if (curLevel > MIN_TX_FIFO_THRESHOLD)
		newLevel--;
	if (newLevel != curLevel)
		/* Update the trigger level */
		OS_REG_WRITE(ah, AR_TXCFG,
			(txcfg &~ AR_FTRIG) | SM(newLevel, AR_FTRIG));

	/* re-enable chip interrupts */
	ar5212SetInterrupts(ah, omask);

	return (newLevel != curLevel);
}

/*
 * Set the properties of the tx queue with the parameters
 * from qInfo.  
 */
HAL_BOOL
ar5212SetTxQueueProps(struct ath_hal *ah, int q, const HAL_TXQ_INFO *qInfo)
{
	struct ath_hal_5212 *ahp = AH5212(ah);
	HAL_CAPABILITIES *pCap = &AH_PRIVATE(ah)->ah_caps;

	if (q >= pCap->halTotalQueues) {
		HALDEBUG(ah, "%s: invalid queue num %u\n", __func__, q);
		return AH_FALSE;
	}
	return ath_hal_setTxQProps(ah, &ahp->ah_txq[q], qInfo);
}

/*
 * Return the properties for the specified tx queue.
 */
HAL_BOOL
ar5212GetTxQueueProps(struct ath_hal *ah, int q, HAL_TXQ_INFO *qInfo)
{
	struct ath_hal_5212 *ahp = AH5212(ah);
	HAL_CAPABILITIES *pCap = &AH_PRIVATE(ah)->ah_caps;


	if (q >= pCap->halTotalQueues) {
		HALDEBUG(ah, "%s: invalid queue num %u\n", __func__, q);
		return AH_FALSE;
	}
	return ath_hal_getTxQProps(ah, qInfo, &ahp->ah_txq[q]);
}

/*
 * Allocate and initialize a tx DCU/QCU combination.
 */
int
ar5212SetupTxQueue(struct ath_hal *ah, HAL_TX_QUEUE type,
	const HAL_TXQ_INFO *qInfo)
{
	struct ath_hal_5212 *ahp = AH5212(ah);
	HAL_TX_QUEUE_INFO *qi;
	HAL_CAPABILITIES *pCap = &AH_PRIVATE(ah)->ah_caps;
	int q, defqflags;

	/* by default enable OK+ERR+DESC+URN interrupts */
	defqflags = HAL_TXQ_TXOKINT_ENABLE
		  | HAL_TXQ_TXERRINT_ENABLE
		  | HAL_TXQ_TXDESCINT_ENABLE
		  | HAL_TXQ_TXURNINT_ENABLE;
	/* XXX move queue assignment to driver */
	switch (type) {
	case HAL_TX_QUEUE_BEACON:
		q = pCap->halTotalQueues-1;	/* highest priority */
		defqflags |= HAL_TXQ_DBA_GATED
		       | HAL_TXQ_CBR_DIS_QEMPTY
		       | HAL_TXQ_ARB_LOCKOUT_GLOBAL
		       | HAL_TXQ_BACKOFF_DISABLE;
		break;
	case HAL_TX_QUEUE_CAB:
		q = pCap->halTotalQueues-2;	/* next highest priority */
		defqflags |= HAL_TXQ_DBA_GATED
		       | HAL_TXQ_CBR_DIS_QEMPTY
		       | HAL_TXQ_CBR_DIS_BEMPTY
		       | HAL_TXQ_ARB_LOCKOUT_GLOBAL
		       | HAL_TXQ_BACKOFF_DISABLE;
		break;
	case HAL_TX_QUEUE_UAPSD:
		q = pCap->halTotalQueues-3;	/* nextest highest priority */
		if (ahp->ah_txq[q].tqi_type != HAL_TX_QUEUE_INACTIVE) {
			HALDEBUG(ah, "%s: no available UAPSD tx queue\n", __func__);
			return -1;
		}
		break;
	case HAL_TX_QUEUE_DATA:
		for (q = 0; q < pCap->halTotalQueues; q++)
			if (ahp->ah_txq[q].tqi_type == HAL_TX_QUEUE_INACTIVE)
				break;
		if (q == pCap->halTotalQueues) {
			HALDEBUG(ah, "%s: no available tx queue\n", __func__);
			return -1;
		}
		break;
	default:
		HALDEBUG(ah, "%s: bad tx queue type %u\n", __func__, type);
		return -1;
	}

	HALDEBUG(ah, "%s: queue %u\n", __func__, q);

	qi = &ahp->ah_txq[q];
	if (qi->tqi_type != HAL_TX_QUEUE_INACTIVE) {
		HALDEBUG(ah, "%s: tx queue %u already active\n", __func__, q);
		return -1;
	}
	OS_MEMZERO(qi, sizeof(HAL_TX_QUEUE_INFO));
	qi->tqi_type = type;
	if (qInfo == AH_NULL) {
		qi->tqi_qflags = defqflags;
		qi->tqi_aifs = INIT_AIFS;
		qi->tqi_cwmin = HAL_TXQ_USEDEFAULT;	/* NB: do at reset */
		qi->tqi_cwmax = INIT_CWMAX;
		qi->tqi_shretry = INIT_SH_RETRY;
		qi->tqi_lgretry = INIT_LG_RETRY;
		qi->tqi_physCompBuf = 0;
	} else {
		qi->tqi_physCompBuf = qInfo->tqi_compBuf;
		(void) ar5212SetTxQueueProps(ah, q, qInfo);
	}
	/* NB: must be followed by ar5212ResetTxQueue */
	return q;
}

/*
 * Update the h/w interrupt registers to reflect a tx q's configuration.
 */
static void
setTxQInterrupts(struct ath_hal *ah, HAL_TX_QUEUE_INFO *qi)
{
	struct ath_hal_5212 *ahp = AH5212(ah);

	HALDEBUG(ah, "%s: tx ok 0x%x err 0x%x desc 0x%x eol 0x%x urn 0x%x\n"
		, __func__
		, ahp->ah_txOkInterruptMask
		, ahp->ah_txErrInterruptMask
		, ahp->ah_txDescInterruptMask
		, ahp->ah_txEolInterruptMask
		, ahp->ah_txUrnInterruptMask
	);

	OS_REG_WRITE(ah, AR_IMR_S0,
		  SM(ahp->ah_txOkInterruptMask, AR_IMR_S0_QCU_TXOK)
		| SM(ahp->ah_txDescInterruptMask, AR_IMR_S0_QCU_TXDESC)
	);
	OS_REG_WRITE(ah, AR_IMR_S1,
		  SM(ahp->ah_txErrInterruptMask, AR_IMR_S1_QCU_TXERR)
		| SM(ahp->ah_txEolInterruptMask, AR_IMR_S1_QCU_TXEOL)
	);
	OS_REG_RMW_FIELD(ah, AR_IMR_S2,
		AR_IMR_S2_QCU_TXURN, ahp->ah_txUrnInterruptMask);
}

/*
 * Free a tx DCU/QCU combination.
 */
HAL_BOOL
ar5212ReleaseTxQueue(struct ath_hal *ah, u_int q)
{
	struct ath_hal_5212 *ahp = AH5212(ah);
	HAL_CAPABILITIES *pCap = &AH_PRIVATE(ah)->ah_caps;
	HAL_TX_QUEUE_INFO *qi;

	if (q >= pCap->halTotalQueues) {
		HALDEBUG(ah, "%s: invalid queue num %u\n", __func__, q);
		return AH_FALSE;
	}
	qi = &ahp->ah_txq[q];
	if (qi->tqi_type == HAL_TX_QUEUE_INACTIVE) {
		HALDEBUG(ah, "%s: inactive queue %u\n", __func__, q);
		return AH_FALSE;
	}

	HALDEBUG(ah, "%s: release queue %u\n", __func__, q);

	qi->tqi_type = HAL_TX_QUEUE_INACTIVE;
	ahp->ah_txOkInterruptMask &= ~(1 << q);
	ahp->ah_txErrInterruptMask &= ~(1 << q);
	ahp->ah_txDescInterruptMask &= ~(1 << q);
	ahp->ah_txEolInterruptMask &= ~(1 << q);
	ahp->ah_txUrnInterruptMask &= ~(1 << q);
	setTxQInterrupts(ah, qi);

	return AH_TRUE;
}

/*
 * Set the retry, aifs, cwmin/max, readyTime regs for specified queue
 * Assumes:
 *  phwChannel has been set to point to the current channel
 */
HAL_BOOL
ar5212ResetTxQueue(struct ath_hal *ah, u_int q)
{
	struct ath_hal_5212 *ahp = AH5212(ah);
	HAL_CAPABILITIES *pCap = &AH_PRIVATE(ah)->ah_caps;
	HAL_CHANNEL_INTERNAL *chan = AH_PRIVATE(ah)->ah_curchan;
	HAL_TX_QUEUE_INFO *qi;
	u_int32_t cwMin, chanCwMin, value, qmisc, dmisc;

	if (q >= pCap->halTotalQueues) {
		HALDEBUG(ah, "%s: invalid queue num %u\n", __func__, q);
		return AH_FALSE;
	}
	qi = &ahp->ah_txq[q];
	if (qi->tqi_type == HAL_TX_QUEUE_INACTIVE) {
		HALDEBUG(ah, "%s: inactive queue %u\n", __func__, q);
		return AH_TRUE;		/* XXX??? */
	}

	HALDEBUG(ah, "%s: reset queue %u\n", __func__, q);

	if (qi->tqi_cwmin == HAL_TXQ_USEDEFAULT) {
		/*
		 * Select cwmin according to channel type.
		 * NB: chan can be NULL during attach
		 */
#ifdef AH_SUPPORT_XR
		if (chan && AH5212(ah)->ah_xrEnable)
			chanCwMin = INIT_CWMIN_XR;
		else
#endif /* AH_SUPPORT_XR */
		if (chan && IS_CHAN_B(chan))
			chanCwMin = INIT_CWMIN_11B;
		else
			chanCwMin = INIT_CWMIN;
		/* make sure that the CWmin is of the form (2^n - 1) */
		for (cwMin = 1; cwMin < chanCwMin; cwMin = (cwMin << 1) | 1)
			;
	} else
		cwMin = qi->tqi_cwmin;

	/* set cwMin/Max and AIFS values */
	OS_REG_WRITE(ah, AR_DLCL_IFS(q),
		  SM(cwMin, AR_D_LCL_IFS_CWMIN)
		| SM(qi->tqi_cwmax, AR_D_LCL_IFS_CWMAX)
		| SM(qi->tqi_aifs, AR_D_LCL_IFS_AIFS));

	/* Set retry limit values */
	OS_REG_WRITE(ah, AR_DRETRY_LIMIT(q), 
		   SM(INIT_SSH_RETRY, AR_D_RETRY_LIMIT_STA_SH)
		 | SM(INIT_SLG_RETRY, AR_D_RETRY_LIMIT_STA_LG)
		 | SM(qi->tqi_lgretry, AR_D_RETRY_LIMIT_FR_LG)
		 | SM(qi->tqi_shretry, AR_D_RETRY_LIMIT_FR_SH)
	);

	/* NB: always enable early termination on the QCU */
	qmisc = AR_Q_MISC_DCU_EARLY_TERM_REQ
	      | SM(AR_Q_MISC_FSP_ASAP, AR_Q_MISC_FSP);

	/* NB: always enable DCU to wait for next fragment from QCU */
	dmisc = AR_D_MISC_FRAG_WAIT_EN;

#ifdef AH_SUPPORT_5311
	if (AH_PRIVATE(ah)->ah_macVersion < AR_SREV_VERSION_OAHU) {
		/* Configure DCU to use the global sequence count */
		dmisc |= AR5311_D_MISC_SEQ_NUM_CONTROL;
	}
#endif
	/* multiqueue support */
	if (qi->tqi_cbrPeriod) {
		OS_REG_WRITE(ah, AR_QCBRCFG(q), 
			  SM(qi->tqi_cbrPeriod,AR_Q_CBRCFG_CBR_INTERVAL)
			| SM(qi->tqi_cbrOverflowLimit, AR_Q_CBRCFG_CBR_OVF_THRESH));
		qmisc = (qmisc &~ AR_Q_MISC_FSP) | AR_Q_MISC_FSP_CBR;
		if (qi->tqi_cbrOverflowLimit)
			qmisc |= AR_Q_MISC_CBR_EXP_CNTR_LIMIT;
	}
	if (qi->tqi_readyTime) {
		OS_REG_WRITE(ah, AR_QRDYTIMECFG(q),
			  SM(qi->tqi_readyTime, AR_Q_RDYTIMECFG_INT)
			| AR_Q_RDYTIMECFG_ENA);
	}
	
	OS_REG_WRITE(ah, AR_DCHNTIME(q),
		  SM(qi->tqi_burstTime, AR_D_CHNTIME_DUR)
		| (qi->tqi_burstTime ? AR_D_CHNTIME_EN : 0));

	if (qi->tqi_readyTime &&
	    (qi->tqi_qflags & HAL_TXQ_RDYTIME_EXP_POLICY_ENABLE))
		qmisc |= AR_Q_MISC_RDYTIME_EXP_POLICY;
	if (qi->tqi_qflags & HAL_TXQ_DBA_GATED)
		qmisc = (qmisc &~ AR_Q_MISC_FSP) | AR_Q_MISC_FSP_DBA_GATED;
	if (MS(qmisc, AR_Q_MISC_FSP) != AR_Q_MISC_FSP_ASAP) {
		/*
		 * These are meangingful only when not scheduled asap.
		 */
		if (qi->tqi_qflags & HAL_TXQ_CBR_DIS_BEMPTY)
			qmisc |= AR_Q_MISC_CBR_INCR_DIS0;
		else
			qmisc &= ~AR_Q_MISC_CBR_INCR_DIS0;
		if (qi->tqi_qflags & HAL_TXQ_CBR_DIS_QEMPTY)
			qmisc |= AR_Q_MISC_CBR_INCR_DIS1;
		else
			qmisc &= ~AR_Q_MISC_CBR_INCR_DIS1;
	}

	if (qi->tqi_qflags & HAL_TXQ_BACKOFF_DISABLE)
		dmisc |= AR_D_MISC_POST_FR_BKOFF_DIS;
	if (qi->tqi_qflags & HAL_TXQ_FRAG_BURST_BACKOFF_ENABLE)
		dmisc |= AR_D_MISC_FRAG_BKOFF_EN;
	if (qi->tqi_qflags & HAL_TXQ_ARB_LOCKOUT_GLOBAL)
		dmisc |= SM(AR_D_MISC_ARB_LOCKOUT_CNTRL_GLOBAL,
			    AR_D_MISC_ARB_LOCKOUT_CNTRL);
	else if (qi->tqi_qflags & HAL_TXQ_ARB_LOCKOUT_INTRA)
		dmisc |= SM(AR_D_MISC_ARB_LOCKOUT_CNTRL_INTRA_FR,
			    AR_D_MISC_ARB_LOCKOUT_CNTRL);
	if (qi->tqi_qflags & HAL_TXQ_IGNORE_VIRTCOL)
		dmisc |= SM(AR_D_MISC_VIR_COL_HANDLING_IGNORE,
			    AR_D_MISC_VIR_COL_HANDLING);
	if (qi->tqi_qflags & HAL_TXQ_SEQNUM_INC_DIS)
		dmisc |= AR_D_MISC_SEQ_NUM_INCR_DIS;

	/*
	 * Fillin type-dependent bits.  Most of this can be
	 * removed by specifying the queue parameters in the
	 * driver; it's here for backwards compatibility.
	 */
	switch (qi->tqi_type) {
	case HAL_TX_QUEUE_BEACON:		/* beacon frames */
		qmisc |= AR_Q_MISC_FSP_DBA_GATED
		      |  AR_Q_MISC_BEACON_USE
		      |  AR_Q_MISC_CBR_INCR_DIS1;

		dmisc |= SM(AR_D_MISC_ARB_LOCKOUT_CNTRL_GLOBAL,
			    AR_D_MISC_ARB_LOCKOUT_CNTRL)
		      |  AR_D_MISC_BEACON_USE
		      |  AR_D_MISC_POST_FR_BKOFF_DIS;
		break;
	case HAL_TX_QUEUE_CAB:			/* CAB  frames */
		/* 
		 * No longer Enable AR_Q_MISC_RDYTIME_EXP_POLICY,
		 * bug #6079.  There is an issue with the CAB Queue
		 * not properly refreshing the Tx descriptor if
		 * the TXE clear setting is used.
		 */
		qmisc |= AR_Q_MISC_FSP_DBA_GATED
		      |  AR_Q_MISC_CBR_INCR_DIS1
		      |  AR_Q_MISC_CBR_INCR_DIS0;

		if (!qi->tqi_readyTime) {
			/*
			 * NB: don't set default ready time if driver
			 * has explicitly specified something.  This is
			 * here solely for backwards compatibility.
			 */
			value = (ahp->ah_beaconInterval
				- (ath_hal_sw_beacon_response_time -
					ath_hal_dma_beacon_response_time)
				- ath_hal_additional_swba_backoff) * 1024;
			OS_REG_WRITE(ah, AR_QRDYTIMECFG(q), value | AR_Q_RDYTIMECFG_ENA);
		}
		dmisc |= SM(AR_D_MISC_ARB_LOCKOUT_CNTRL_GLOBAL,
			    AR_D_MISC_ARB_LOCKOUT_CNTRL);
		break;
	default:			/* NB: silence compiler */
		break;
	}

#ifndef AH_DISABLE_WME
	/*
	 * Yes, this is a hack and not the right way to do it, but
	 * it does get the lockout bits and backoff set for the
	 * high-pri WME queues for testing.  We need to either extend
	 * the meaning of queueInfo->mode, or create something like
	 * queueInfo->dcumode.
	 */
	if (qi->tqi_intFlags & HAL_TXQ_USE_LOCKOUT_BKOFF_DIS) {
		dmisc |= SM(AR_D_MISC_ARB_LOCKOUT_CNTRL_GLOBAL,
			    AR_D_MISC_ARB_LOCKOUT_CNTRL)
		      |  AR_D_MISC_POST_FR_BKOFF_DIS;
	}
#endif
	OS_REG_WRITE(ah, AR_QMISC(q), qmisc);
	OS_REG_WRITE(ah, AR_DMISC(q), dmisc);

	/* Setup compression scratchpad buffer */
	/* 
	 * XXX: calling this asynchronously to queue operation can
	 *      cause unexpected behavior!!!
	 */
	if (qi->tqi_physCompBuf) {
		HALASSERT(qi->tqi_type == HAL_TX_QUEUE_DATA ||
			  qi->tqi_type == HAL_TX_QUEUE_UAPSD);
		OS_REG_WRITE(ah, AR_Q_CBBS, (80 + 2*q));
		OS_REG_WRITE(ah, AR_Q_CBBA, qi->tqi_physCompBuf);
		OS_REG_WRITE(ah, AR_Q_CBC,  HAL_COMP_BUF_MAX_SIZE/1024);
		OS_REG_WRITE(ah, AR_Q0_MISC + 4*q,
			     OS_REG_READ(ah, AR_Q0_MISC + 4*q)
			     | AR_Q_MISC_QCU_COMP_EN);
	}
	
	/*
	 * Always update the secondary interrupt mask registers - this
	 * could be a new queue getting enabled in a running system or
	 * hw getting re-initialized during a reset!
	 *
	 * Since we don't differentiate between tx interrupts corresponding
	 * to individual queues - secondary tx mask regs are always unmasked;
	 * tx interrupts are enabled/disabled for all queues collectively
	 * using the primary mask reg
	 */
	if (qi->tqi_qflags & HAL_TXQ_TXOKINT_ENABLE)
		ahp->ah_txOkInterruptMask |= 1 << q;
	else
		ahp->ah_txOkInterruptMask &= ~(1 << q);
	if (qi->tqi_qflags & HAL_TXQ_TXERRINT_ENABLE)
		ahp->ah_txErrInterruptMask |= 1 << q;
	else
		ahp->ah_txErrInterruptMask &= ~(1 << q);
	if (qi->tqi_qflags & HAL_TXQ_TXDESCINT_ENABLE)
		ahp->ah_txDescInterruptMask |= 1 << q;
	else
		ahp->ah_txDescInterruptMask &= ~(1 << q);
	if (qi->tqi_qflags & HAL_TXQ_TXEOLINT_ENABLE)
		ahp->ah_txEolInterruptMask |= 1 << q;
	else
		ahp->ah_txEolInterruptMask &= ~(1 << q);
	if (qi->tqi_qflags & HAL_TXQ_TXURNINT_ENABLE)
		ahp->ah_txUrnInterruptMask |= 1 << q;
	else
		ahp->ah_txUrnInterruptMask &= ~(1 << q);
	setTxQInterrupts(ah, qi);

	return AH_TRUE;
}

/*
 * Get the TXDP for the specified queue
 */
u_int32_t
ar5212GetTxDP(struct ath_hal *ah, u_int q)
{
	HALASSERT(q < AH_PRIVATE(ah)->ah_caps.halTotalQueues);
	return OS_REG_READ(ah, AR_QTXDP(q));
}

/*
 * Set the TxDP for the specified queue
 */
HAL_BOOL
ar5212SetTxDP(struct ath_hal *ah, u_int q, u_int32_t txdp)
{
	HALASSERT(q < AH_PRIVATE(ah)->ah_caps.halTotalQueues);
	HALASSERT(AH5212(ah)->ah_txq[q].tqi_type != HAL_TX_QUEUE_INACTIVE);

	/*
	 * Make sure that TXE is deasserted before setting the TXDP.  If TXE
	 * is still asserted, setting TXDP will have no effect.
	 */
	HALASSERT((OS_REG_READ(ah, AR_Q_TXE) & (1 << q)) == 0);

	OS_REG_WRITE(ah, AR_QTXDP(q), txdp);

	return AH_TRUE;
}

/*
 * Set Transmit Enable bits for the specified queue
 */
HAL_BOOL
ar5212StartTxDma(struct ath_hal *ah, u_int q)
{
	HALASSERT(q < AH_PRIVATE(ah)->ah_caps.halTotalQueues);

	HALASSERT(AH5212(ah)->ah_txq[q].tqi_type != HAL_TX_QUEUE_INACTIVE);

	HALDEBUGn(ah, 2, "%s: queue %u\n", __func__, q);

	/* Check to be sure we're not enabling a q that has its TXD bit set. */
	HALASSERT((OS_REG_READ(ah, AR_Q_TXD) & (1 << q)) == 0);

	OS_REG_WRITE(ah, AR_Q_TXE, 1 << q);
	return AH_TRUE;
}

/*
 * Return the number of pending frames or 0 if the specified
 * queue is stopped.
 */
u_int32_t
ar5212NumTxPending(struct ath_hal *ah, u_int q)
{
	u_int32_t npend;

	HALASSERT(q < AH_PRIVATE(ah)->ah_caps.halTotalQueues);
	HALASSERT(AH5212(ah)->ah_txq[q].tqi_type != HAL_TX_QUEUE_INACTIVE);

	npend = OS_REG_READ(ah, AR_QSTS(q)) & AR_Q_STS_PEND_FR_CNT;
	if (npend == 0) {
		/*
		 * Pending frame count (PFC) can momentarily go to zero
		 * while TXE remains asserted.  In other words a PFC of
		 * zero is not sufficient to say that the queue has stopped.
		 */
		if (OS_REG_READ(ah, AR_Q_TXE) & (1 << q))
			npend = 1;		/* arbitrarily return 1 */
	}
#ifdef DEBUG
	if (npend && (AH5212(ah)->ah_txq[q].tqi_type == HAL_TX_QUEUE_CAB)) {
		if (OS_REG_READ(ah, AR_Q_RDYTIMESHDN) & (1 << q)) {
			isrPrintf("RTSD on CAB queue\n");
			/* Clear the ReadyTime shutdown status bits */
			OS_REG_WRITE(ah, AR_Q_RDYTIMESHDN, 1 << q);
		}
	}
#endif
	return npend;
}

/*
 * Stop transmit on the specified queue
 */
HAL_BOOL
ar5212StopTxDma(struct ath_hal *ah, u_int q)
{
	u_int i;
	u_int wait;

	HALASSERT(q < AH_PRIVATE(ah)->ah_caps.halTotalQueues);

	HALASSERT(AH5212(ah)->ah_txq[q].tqi_type != HAL_TX_QUEUE_INACTIVE);

	OS_REG_WRITE(ah, AR_Q_TXD, 1 << q);
	for (i = 1000; i != 0; i--) {
		if (ar5212NumTxPending(ah, q) == 0)
			break;
		OS_DELAY(100);        /* XXX get actual value */
	}
#ifdef AH_DEBUG
	if (i == 0) {
		HALDEBUG(ah, "%s: queue %u DMA did not stop in 100 msec\n",
			__func__, q);
		HALDEBUG(ah, "%s: QSTS 0x%x Q_TXE 0x%x Q_TXD 0x%x Q_CBR 0x%x\n"
			, __func__
			, OS_REG_READ(ah, AR_QSTS(q))
			, OS_REG_READ(ah, AR_Q_TXE)
			, OS_REG_READ(ah, AR_Q_TXD)
			, OS_REG_READ(ah, AR_QCBRCFG(q))
		);
		HALDEBUG(ah, "%s: Q_MISC 0x%x Q_RDYTIMECFG 0x%x Q_RDYTIMESHDN 0x%x\n"
			, __func__
			, OS_REG_READ(ah, AR_QMISC(q))
			, OS_REG_READ(ah, AR_QRDYTIMECFG(q))
			, OS_REG_READ(ah, AR_Q_RDYTIMESHDN)
		);
	}
#endif /* AH_DEBUG */

	/* 2413+ and up can kill packets at the PCU level */
	if (ar5212NumTxPending(ah, q) && (IS_2413(ah) || IS_5413(ah) || IS_2417(ah) || IS_2425(ah))) {
		u_int32_t tsfLow, j;
		
		HALDEBUG(ah, "%s: Num of pending TX Frames %d on Q %d\n",
			 __func__, ar5212NumTxPending(ah, q), q);
		
		/* Kill last PCU Tx Frame */
		/* TODO - save off and restore current values of Q1/Q2? */
		for (j = 0; j < 2; j++) {
			tsfLow = OS_REG_READ(ah, AR_TSF_L32);
			OS_REG_WRITE(ah, AR_QUIET2, SM(100, AR_QUIET2_QUIET_PER) |
				     SM(10, AR_QUIET2_QUIET_DUR));
			OS_REG_WRITE(ah, AR_QUIET1, AR_QUIET1_QUIET_ENABLE |
				     SM(tsfLow >> 10, AR_QUIET1_NEXT_QUIET));
			if ((OS_REG_READ(ah, AR_TSF_L32) >> 10) == (tsfLow >> 10)) {
				break;
			}
			HALDEBUG(ah, "%s: TSF have moved while trying to set quiet time TSF: 0x%08x\n", __func__, tsfLow);
			HALASSERT(j < 1); /* TSF shouldn't count twice or reg access is taking forever */
		}
		
		OS_REG_SET_BIT(ah, AR_DIAG_SW, AR_DIAG_CHAN_IDLE);
		
		/* Allow the quiet mechanism to do its work */
		OS_DELAY(200);
		OS_REG_CLR_BIT(ah, AR_QUIET1, AR_QUIET1_QUIET_ENABLE);
		
		/* Give at least 1 millisec more to wait */
		wait = 100;
		
		/* Verify all transmit is dead */
		while (ar5212NumTxPending(ah, q)) {
			if ((--wait) == 0) {
#ifdef AH_DEBUG
				HALDEBUG(ah, "%s: Failed to stop Tx DMA in %d msec after killing last frame\n",
					 __func__, wait);
#endif
				break;
			}
			OS_DELAY(10);
		}
		
		OS_REG_CLR_BIT(ah, AR_DIAG_SW, AR_DIAG_CHAN_IDLE);
	}

	OS_REG_WRITE(ah, AR_Q_TXD, 0);
	return (i != 0);
}

/*
 * Descriptor Access Functions
 */

#define	VALID_PKT_TYPES \
	((1<<HAL_PKT_TYPE_NORMAL)|(1<<HAL_PKT_TYPE_ATIM)|\
	 (1<<HAL_PKT_TYPE_PSPOLL)|(1<<HAL_PKT_TYPE_PROBE_RESP)|\
	 (1<<HAL_PKT_TYPE_BEACON))
#define	isValidPktType(_t)	((1<<(_t)) & VALID_PKT_TYPES)
#define	VALID_TX_RATES \
	((1<<0x0b)|(1<<0x0f)|(1<<0x0a)|(1<<0x0e)|(1<<0x09)|(1<<0x0d)|\
	 (1<<0x08)|(1<<0x0c)|(1<<0x1b)|(1<<0x1a)|(1<<0x1e)|(1<<0x19)|\
	 (1<<0x1d)|(1<<0x18)|(1<<0x1c))
#define	isValidTxRate(_r)	((1<<(_r)) & VALID_TX_RATES)

HAL_BOOL
ar5212SetupTxDesc(struct ath_hal *ah, struct ath_desc *ds,
	u_int pktLen,
	u_int hdrLen,
	HAL_PKT_TYPE type,
	u_int txPower,
	u_int txRate0, u_int txTries0,
	u_int keyIx,
	u_int antMode,
	u_int flags,
	u_int rtsctsRate,
	u_int rtsctsDuration,
	u_int compicvLen,
	u_int compivLen,
	u_int comp)
{
#define	RTSCTS	(HAL_TXDESC_RTSENA|HAL_TXDESC_CTSENA)
	struct ar5212_desc *ads = AR5212DESC(ds);
	struct ath_hal_5212 *ahp = AH5212(ah);

	(void) hdrLen;

	HALASSERT(txTries0 != 0);
	HALASSERT(isValidPktType(type));
	HALASSERT(isValidTxRate(txRate0));
	HALASSERT((flags & RTSCTS) != RTSCTS);
	/* XXX validate antMode */

        txPower = (txPower + ahp->ah_txPowerIndexOffset );
        if(txPower > 63)  txPower=63;

	ads->ds_ctl0 = (pktLen & AR_FrameLen)
		     | (txPower << AR_XmitPower_S)
		     | (flags & HAL_TXDESC_VEOL ? AR_VEOL : 0)
		     | (flags & HAL_TXDESC_CLRDMASK ? AR_ClearDestMask : 0)
		     | SM(antMode, AR_AntModeXmit)
		     | (flags & HAL_TXDESC_INTREQ ? AR_TxInterReq : 0)
		     ;
	ads->ds_ctl1 = (type << AR_FrmType_S)
		     | (flags & HAL_TXDESC_NOACK ? AR_NoAck : 0)
                     | (comp << AR_CompProc_S)
                     | (compicvLen << AR_CompICVLen_S)
                     | (compivLen << AR_CompIVLen_S)
                     ;
	ads->ds_ctl2 = SM(txTries0, AR_XmitDataTries0)
		     | (flags & HAL_TXDESC_DURENA ? AR_DurUpdateEna : 0)
		     ;
	ads->ds_ctl3 = (txRate0 << AR_XmitRate0_S)
		     ;
	if (keyIx != HAL_TXKEYIX_INVALID) {
		/* XXX validate key index */
		ads->ds_ctl1 |= SM(keyIx, AR_DestIdx);
		ads->ds_ctl0 |= AR_DestIdxValid;
	}
	if (flags & RTSCTS) {
		if (!isValidTxRate(rtsctsRate)) {
			HALDEBUG(ah, "%s: invalid rts/cts rate 0x%x\n",
				__func__, rtsctsRate);
			return AH_FALSE;
		}
		/* XXX validate rtsctsDuration */
		ads->ds_ctl0 |= (flags & HAL_TXDESC_CTSENA ? AR_CTSEnable : 0)
			     | (flags & HAL_TXDESC_RTSENA ? AR_RTSCTSEnable : 0)
			     ;
		ads->ds_ctl2 |= SM(rtsctsDuration, AR_RTSCTSDuration);
		ads->ds_ctl3 |= (rtsctsRate << AR_RTSCTSRate_S);
	}
	return AH_TRUE;
#undef RTSCTS
}

HAL_BOOL
ar5212SetupXTxDesc(struct ath_hal *ah, struct ath_desc *ds,
	u_int txRate1, u_int txTries1,
	u_int txRate2, u_int txTries2,
	u_int txRate3, u_int txTries3)
{
	struct ar5212_desc *ads = AR5212DESC(ds);

	if (txTries1 && txRate1) {
		HALASSERT(isValidTxRate(txRate1));
		ads->ds_ctl2 |= SM(txTries1, AR_XmitDataTries1)
			     |  AR_DurUpdateEna
			     ;
		ads->ds_ctl3 |= (txRate1 << AR_XmitRate1_S);
	}
	if (txTries2 && txRate2) {
		HALASSERT(isValidTxRate(txRate2));
		ads->ds_ctl2 |= SM(txTries2, AR_XmitDataTries2)
			     |  AR_DurUpdateEna
			     ;
		ads->ds_ctl3 |= (txRate2 << AR_XmitRate2_S);
	}
	if (txTries3 && txRate3) {
		HALASSERT(isValidTxRate(txRate3));
		ads->ds_ctl2 |= SM(txTries3, AR_XmitDataTries3)
			     |  AR_DurUpdateEna
			     ;
		ads->ds_ctl3 |= (txRate3 << AR_XmitRate3_S);
	}
	return AH_TRUE;
}

void
ar5212IntrReqTxDesc(struct ath_hal *ah, struct ath_desc *ds)
{
	struct ar5212_desc *ads = AR5212DESC(ds);

#ifdef AH_NEED_DESC_SWAP
	ads->ds_ctl0 |= __bswap32(AR_TxInterReq);
#else
	ads->ds_ctl0 |= AR_TxInterReq;
#endif
}

HAL_BOOL
ar5212FillTxDesc(struct ath_hal *ah, struct ath_desc *ds,
	u_int segLen, HAL_BOOL firstSeg, HAL_BOOL lastSeg,
	const struct ath_desc *ds0)
{
	struct ar5212_desc *ads = AR5212DESC(ds);

	HALASSERT((segLen &~ AR_BufLen) == 0);

	if (firstSeg) {
		/*
		 * First descriptor, don't clobber xmit control data
		 * setup by ar5212SetupTxDesc.
		 */
		ads->ds_ctl1 |= segLen | (lastSeg ? 0 : AR_More);
	} else if (lastSeg) {		/* !firstSeg && lastSeg */
		/*
		 * Last descriptor in a multi-descriptor frame,
		 * copy the multi-rate transmit parameters from
		 * the first frame for processing on completion. 
		 */
		ads->ds_ctl0 = 0;
		ads->ds_ctl1 = segLen;
#ifdef AH_NEED_DESC_SWAP
		ads->ds_ctl2 = __bswap32(AR5212DESC_CONST(ds0)->ds_ctl2);
		ads->ds_ctl3 = __bswap32(AR5212DESC_CONST(ds0)->ds_ctl3);
#else
		ads->ds_ctl2 = AR5212DESC_CONST(ds0)->ds_ctl2;
		ads->ds_ctl3 = AR5212DESC_CONST(ds0)->ds_ctl3;
#endif
	} else {			/* !firstSeg && !lastSeg */
		/*
		 * Intermediate descriptor in a multi-descriptor frame.
		 */
		ads->ds_ctl0 = 0;
		ads->ds_ctl1 = segLen | AR_More;
		ads->ds_ctl2 = 0;
		ads->ds_ctl3 = 0;
	}
	ads->ds_txstatus0 = ads->ds_txstatus1 = 0;
	return AH_TRUE;
}

#ifdef AH_NEED_DESC_SWAP
/* Swap transmit descriptor */
static __inline void
ar5212SwapTxDesc(struct ath_desc *ds)
{
	ds->ds_data = __bswap32(ds->ds_data);
        ds->ds_ctl0 = __bswap32(ds->ds_ctl0);
        ds->ds_ctl1 = __bswap32(ds->ds_ctl1);
        ds->ds_hw[0] = __bswap32(ds->ds_hw[0]);
        ds->ds_hw[1] = __bswap32(ds->ds_hw[1]);
        ds->ds_hw[2] = __bswap32(ds->ds_hw[2]);
        ds->ds_hw[3] = __bswap32(ds->ds_hw[3]);
}
#endif

/*
 * Processing of HW TX descriptor.
 */
HAL_STATUS
ar5212ProcTxDesc(struct ath_hal *ah,
	struct ath_desc *ds, struct ath_tx_status *ts)
{
	struct ar5212_desc *ads = AR5212DESC(ds);

#ifdef AH_NEED_DESC_SWAP
	if ((ads->ds_txstatus1 & __bswap32(AR_Done)) == 0)
                return HAL_EINPROGRESS;

	ar5212SwapTxDesc(ds);
#else
	if ((ads->ds_txstatus1 & AR_Done) == 0)
		return HAL_EINPROGRESS;
#endif

	/* Update software copies of the HW status */
	ts->ts_seqnum = MS(ads->ds_txstatus1, AR_SeqNum);
	ts->ts_tstamp = MS(ads->ds_txstatus0, AR_SendTimestamp);
	ts->ts_status = 0;
	if ((ads->ds_txstatus0 & AR_FrmXmitOK) == 0) {
		if (ads->ds_txstatus0 & AR_ExcessiveRetries)
			ts->ts_status |= HAL_TXERR_XRETRY;
		if (ads->ds_txstatus0 & AR_Filtered)
			ts->ts_status |= HAL_TXERR_FILT;
		if (ads->ds_txstatus0 & AR_FIFOUnderrun)
			ts->ts_status |= HAL_TXERR_FIFO;
	}
	/*
	 * Extract the transmit rate used and mark the rate as
	 * ``alternate'' if it wasn't the series 0 rate.
	 */
	ts->ts_finaltsi = MS(ads->ds_txstatus1, AR_FinalTSIndex);
	switch (ts->ts_finaltsi) {
	case 0:
		ts->ts_rate = MS(ads->ds_ctl3, AR_XmitRate0);
		break;
	case 1:
		ts->ts_rate = MS(ads->ds_ctl3, AR_XmitRate1) |
			HAL_TXSTAT_ALTRATE;
		break;
	case 2:
		ts->ts_rate = MS(ads->ds_ctl3, AR_XmitRate2) |
			HAL_TXSTAT_ALTRATE;
		break;
	case 3:
		ts->ts_rate = MS(ads->ds_ctl3, AR_XmitRate3) |
			HAL_TXSTAT_ALTRATE;
		break;
	}
	ts->ts_rssi = MS(ads->ds_txstatus1, AR_AckSigStrength);
	ts->ts_shortretry = MS(ads->ds_txstatus0, AR_RTSFailCnt);
	ts->ts_longretry = MS(ads->ds_txstatus0, AR_DataFailCnt);
	/*
	 * The retry count has the number of un-acked tries for the
	 * final series used.  When doing multi-rate retry we must
	 * fixup the retry count by adding in the try counts for
	 * each series that was fully-processed.  Beware that this
	 * takes values from the try counts in the final descriptor.
	 * These are not required by the hardware.  We assume they
	 * are placed there by the driver as otherwise we have no
	 * access and the driver can't do the calculation because it
	 * doesn't know the descriptor format.
	 */
	switch (ts->ts_finaltsi) {
	case 3: ts->ts_longretry += MS(ads->ds_ctl2, AR_XmitDataTries2);
	case 2: ts->ts_longretry += MS(ads->ds_ctl2, AR_XmitDataTries1);
	case 1: ts->ts_longretry += MS(ads->ds_ctl2, AR_XmitDataTries0);
	}
	ts->ts_virtcol = MS(ads->ds_txstatus0, AR_VirtCollCnt);
	ts->ts_antenna = (ads->ds_txstatus1 & AR_XmitAtenna ? 2 : 1);

	return HAL_OK;
}

/*
 * Determine which tx queues need interrupt servicing.
 */
void
ar5212GetTxIntrQueue(struct ath_hal *ah, u_int32_t *txqs)
{
	struct ath_hal_5212 *ahp = AH5212(ah);
	*txqs &= ahp->ah_intrTxqs;
	ahp->ah_intrTxqs &= ~(*txqs);
}
#endif /* AH_SUPPORT_AR5212 */
