/* $Id */

/*
 ** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
 ** Copyright (C) 2011-2013 Sourcefire, Inc.
 **
 **
 ** This program is free software; you can redistribute it and/or modify
 ** it under the terms of the GNU General Public License Version 2 as
 ** published by the Free Software Foundation.  You may not use, modify or
 ** distribute this program under any other version of the GNU General
 ** Public License.
 **
 ** This program is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 **
 ** You should have received a copy of the GNU General Public License
 ** along with this program; if not, write to the Free Software
 ** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */


/*
 * Reputation preprocessor
 *
 * This is the main entry point for this preprocessor
 *
 * Author: Hui Cao
 * Date: 06-01-2011
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif  /* HAVE_CONFIG_H */
#include "sf_types.h"
#include "sf_snort_packet.h"
#include "sf_dynamic_preprocessor.h"
#include "sf_snort_plugin_api.h"
#include "snort_debug.h"

#include "preprocids.h"
#include "spp_reputation.h"
#include "reputation_config.h"
#include "reputation_utils.h"

#include  <assert.h>
#include <stdio.h>
#include <syslog.h>
#include <string.h>
#ifndef WIN32
#include <strings.h>
#include <sys/time.h>
#endif
#include <stdlib.h>
#include <ctype.h>
#ifdef SHARED_REP
#include "./shmem/shmem_mgmt.h"
#endif
#include "profiler.h"
#ifdef PERF_PROFILING
PreprocStats reputationPerfStats;
#endif


const int MAJOR_VERSION = 1;
const int MINOR_VERSION = 1;
const int BUILD_VERSION = 1;

const char *PREPROC_NAME = "SF_REPUTATION";

#define SetupReputation DYNAMIC_PREPROC_SETUP

/*
 * Function prototype(s)
 */
static void ReputationInit( struct _SnortConfig *, char* );
static int ReputationCheckConfig(struct _SnortConfig *);
static inline void ReputationProcess(SFSnortPacket *);
static void ReputationMain( void*, void* );
static void ReputationFreeConfig(tSfPolicyUserContextId);
static void ReputationPrintStats(int);
static void ReputationCleanExit(int, void *);
static inline IPrepInfo*  ReputationLookup(snort_ip_p ip);
static inline IPdecision GetReputation(IPrepInfo *, SFSnortPacket *, uint32_t *);

#ifdef SHARED_REP
Swith_State switch_state = NO_SWITCH;
int available_segment = NO_DATASEG;
static void ReputationMaintenanceCheck(int, void *);
#endif
/********************************************************************
 * Global variables
 ********************************************************************/
int totalNumEntries = 0;
Reputation_Stats reputation_stats;
ReputationConfig *reputation_eval_config;
tSfPolicyUserContextId reputation_config;
ReputationConfig *pDefaultPolicyConfig = NULL;

#ifdef SNORT_RELOAD
static void ReputationReload(struct _SnortConfig *, char *, void **);
static void * ReputationReloadSwap(struct _SnortConfig *, void *);
static void ReputationReloadSwapFree(void *);
static int ReputationReloadVerify(struct _SnortConfig *, void *);
#endif


/* Called at preprocessor setup time. Links preprocessor keyword
 * to corresponding preprocessor initialization function.
 *
 * PARAMETERS:  None.
 *
 * RETURNS:     Nothing.
 *
 */
void SetupReputation(void)
{
    /* Link preprocessor keyword to initialization function
     * in the preprocessor list. */
#ifndef SNORT_RELOAD
    _dpd.registerPreproc( "reputation", ReputationInit );
#else
    _dpd.registerPreproc("reputation", ReputationInit, ReputationReload,
            ReputationReloadVerify, ReputationReloadSwap,
            ReputationReloadSwapFree);
#endif
}
#ifdef SHARED_REP
static int Reputation_MgmtInfo(uint16_t type, const uint8_t *data,
    uint32_t length, void **new_config, char *statusBuf, int statusBufLen)
{
    ShmemMgmtInfo(statusBuf, statusBufLen);

    return 0;
}

static int Reputation_Lookup(uint16_t type, const uint8_t *data, uint32_t length, void **new_config,
        char *statusBuf, int statusBufLen)
{
    snort_ip addr;
    IPrepInfo *repInfo = NULL;
    char *tokstr, *save, *data_copy;
    CSMessageDataHeader *msg_hdr = (CSMessageDataHeader *)data;

    statusBuf[0] = 0;

    if (length <= sizeof(*msg_hdr))
    {
        return -1;
    }
    length -= sizeof(*msg_hdr);
    if (length != (uint32_t)ntohs(msg_hdr->length))
    {
        return -1;
    }

    data += sizeof(*msg_hdr);
    data_copy = malloc(length + 1);
    if (data_copy == NULL)
    {
        return -1;
    }
    memcpy(data_copy, data, length);
    data_copy[length] = 0;

    tokstr = strtok_r(data_copy, " \t\n", &save);
    if (tokstr == NULL)
    {
        free(data_copy);
        return -1;
    }

    /* Convert tokstr to sfip type */
    if (sfip_pton(tokstr, IP_ARG(addr)))
    {
        free(data_copy);
        return -1;
    }

    /* Get the reputation info */
    repInfo = ReputationLookup(IP_ARG(addr));
    if (!repInfo)
    {
        snprintf(statusBuf, statusBufLen,
            "Reputation Info: Error doing lookup");
        free(data_copy);
        return -1;
    }

    /* Are we looking to obtain the decision? */
    tokstr = strtok_r(NULL, " \t\n", &save);
    if (tokstr)
    {
        uint32_t listid;
        char *decision;
#ifdef DAQ_PKTHDR_UNKNOWN
        int zone = atoi(tokstr);
#endif

        SFSnortPacket p;
#ifdef DAQ_PKTHDR_UNKNOWN
        DAQ_PktHdr_t hdr;
        p.pkt_header = &hdr;
        hdr.ingress_group = zone;
#else
        p.pkt_header = NULL;
#endif

        switch (GetReputation(repInfo, &p, &listid))
        {
            case DECISION_NULL:
            decision = "DECISION_NULL";
            break;

            case BLACKLISTED:
            decision = "BLACKLISTED";
            break;

            case WHITELISTED_UNBLACK:
            decision = "WHITELISTED UNBLACK";
            break;

            case MONITORED:
            decision = "MONITORED";
            break;

            case WHITELISTED_TRUST:
            decision = "WHITELISTED TRUST";
            break;

            default:
            decision = "UNKNOWN";
            break;
        }

        snprintf(statusBuf, statusBufLen,
            "Reputation Info: %s in list %d"
#ifdef DAQ_PKTHDR_UNKNOWN
            " from zone %d"
#endif
            ,decision, listid
#ifdef DAQ_PKTHDR_UNKNOWN
            ,zone
#endif
            );
    }
    else
    {
        ReputationRepInfo(repInfo,
            (uint8_t *)reputation_eval_config->iplist,
            statusBuf, statusBufLen);
    }

    free(data_copy);
    return 0;
}

static int Reputation_PreControl(uint16_t type, const uint8_t *data, uint32_t length, void **new_config,
        char *statusBuf, int statusBufLen)
{
    ReputationConfig *pDefaultPolicyConfig = NULL;
    ReputationConfig *nextConfig = NULL;

    statusBuf[0] = 0;

    if (SWITCHING == switch_state )
        return -1;

    pDefaultPolicyConfig = (ReputationConfig *)sfPolicyUserDataGetDefault(reputation_config);

    if (!pDefaultPolicyConfig)
    {
        *new_config = NULL;
        return -1;
    }

    nextConfig = (ReputationConfig *)calloc(1, sizeof(ReputationConfig));

    if (!nextConfig)
    {
        *new_config = NULL;
        return -1;
    }

    switch_state = SWITCHING;

    nextConfig->segment_version = NO_DATASEG;
    nextConfig->memcap = pDefaultPolicyConfig->memcap;
    nextConfig->statusBuf = statusBuf;
    nextConfig->statusBuf_len = statusBufLen;
    reputation_shmem_config = nextConfig;

    if ((available_segment = LoadSharedMemDataSegmentForWriter(RELOAD)) >= 0)
    {
        *new_config = nextConfig;
        nextConfig->segment_version = available_segment;
        _dpd.logMsg("    Reputation Preprocessor: Received segment %d\n",
                available_segment);
        if (!statusBuf[0])
            snprintf(statusBuf,statusBufLen, "Reputation Preprocessor: Received segment %d successful", available_segment);
    }
    else
    {
        *new_config = NULL;
        free(nextConfig);
        switch_state = NO_SWITCH;
        return -1;
    }
    return 0;
}

static int Reputation_Control(uint16_t type, void *new_config, void **old_config)
{
    ReputationConfig *config = (ReputationConfig *) new_config;

    if (NULL != config)
    {
        SwitchToActiveSegment(config->segment_version, &IPtables);
        DEBUG_WRAP(DebugMessage(DEBUG_REPUTATION,"***Switched to segment %d\n",
                config->segment_version));
        *old_config = config;
        return 0;
    }
    return -1;
}

static void Reputation_PostControl(uint16_t type, void *old_config, struct _THREAD_ELEMENT *te, ControlDataSendFunc f)
{
    ReputationConfig *config = (ReputationConfig *) old_config;
    ReputationConfig *pDefaultPolicyConfig = NULL;

    pDefaultPolicyConfig = (ReputationConfig *)sfPolicyUserDataGetDefault(reputation_config);

    if (!pDefaultPolicyConfig)
    {
        return;
    }

    UnmapInactiveSegments();

    pDefaultPolicyConfig->memCapReached = config->memCapReached;
    pDefaultPolicyConfig->segment_version = config->segment_version;
    pDefaultPolicyConfig->memsize = config->memsize;
    pDefaultPolicyConfig->numEntries = config->numEntries;
    pDefaultPolicyConfig->iplist = config->iplist;
    pDefaultPolicyConfig->statusBuf = NULL;
    reputation_shmem_config = pDefaultPolicyConfig;
    switch_state = SWITCHED;
    free(config);

}

static void ReputationMaintenanceCheck(int signal, void *data)
{
    DEBUG_WRAP(DebugMessage(DEBUG_REPUTATION, "Reputation Preprocessor Maintenance!\n"););
    PrintShmemMgmtInfo();
    if (SHMEM_SERVER_ID == _dpd.getSnortInstance())
    {
        ManageUnusedSegments();
        /*check whether new shared memory has been applied. If yes, release the old one*/
        if ((SWITCHED == switch_state) && reputation_eval_config &&
                (reputation_eval_config->iplist == (table_flat_t *)*IPtables))
        {
            _dpd.logMsg("    Reputation Preprocessor: Instance %d switched to segment_version %d\n",
                    _dpd.getSnortInstance(), available_segment);
            UnmapInactiveSegments();
            switch_state = NO_SWITCH;
        }
    }
    else
    {
        if ((NO_SWITCH == switch_state)&&((available_segment = CheckForSharedMemSegment()) >= 0))
        {
            DEBUG_WRAP(DebugMessage(DEBUG_REPUTATION,"***Switched to segment_version %d ",available_segment););
            SwitchToActiveSegment(available_segment, &IPtables);
            switch_state = SWITCHED;

        }
        /*check whether new shared memory has been applied. If yes, release the old one*/
        else if ((SWITCHED == switch_state) && reputation_eval_config &&
                (reputation_eval_config->iplist == (table_flat_t *)*IPtables))
        {
            _dpd.logMsg("    Reputation Preprocessor: Instance %d switched to segment_version %d\n",
                    _dpd.getSnortInstance(), available_segment);
            UnmapInactiveSegments();
            switch_state = NO_SWITCH;
        }
    }
}

/*Switch for idle*/
static void ReputationShmemSwitch(void)
{
    if (switch_state == NO_SWITCH)
        return;

    reputation_eval_config = sfPolicyUserDataGetDefault(reputation_config);

    if (reputation_eval_config)
        reputation_eval_config->iplist = (table_flat_t *)*IPtables;
}

void SetupReputationUpdate(uint32_t updateInterval)
{
    _dpd.addPeriodicCheck(ReputationMaintenanceCheck,NULL, PRIORITY_FIRST, PP_REPUTATION, updateInterval);
    _dpd.registerIdleHandler(ReputationShmemSwitch);
    /*Only writer or server has control channel*/
    if (SHMEM_SERVER_ID == _dpd.getSnortInstance())
    {
        _dpd.controlSocketRegisterHandler(CS_TYPE_REPUTATION_SHAREMEM,
                &Reputation_PreControl, &Reputation_Control, &Reputation_PostControl);
        _dpd.controlSocketRegisterHandler(CS_TYPE_REPUTATION_SHAREMEM_LOOKUP,
                &Reputation_Lookup, NULL, NULL);
        _dpd.controlSocketRegisterHandler(CS_TYPE_REPUTATION_SHAREMEM_MGMT_INFO,
                &Reputation_MgmtInfo, NULL, NULL);
    }

}
#endif

/* Initializes the Reputation preprocessor module and registers
 * it in the preprocessor list.
 *
 * PARAMETERS:
 *
 * argp:   Pointer to argument string to process for configuration data.
 *
 * RETURNS:  Nothing.
 */
static void ReputationInit(struct _SnortConfig *sc, char *argp)
{
    tSfPolicyId policy_id = _dpd.getParserPolicy(sc);
    ReputationConfig *pDefaultPolicyConfig = NULL;
    ReputationConfig *pPolicyConfig = NULL;


    if (reputation_config == NULL)
    {
        /*create a context*/
        reputation_config = sfPolicyConfigCreate();
        if (reputation_config == NULL)
        {
            DynamicPreprocessorFatalMessage("Failed to allocate memory "
                    "for Reputation config.\n");
        }

        _dpd.addPreprocConfCheck(sc, ReputationCheckConfig);
        _dpd.registerPreprocStats(REPUTATION_NAME, ReputationPrintStats);
        _dpd.addPreprocExit(ReputationCleanExit, NULL, PRIORITY_LAST, PP_REPUTATION);

#ifdef PERF_PROFILING
        _dpd.addPreprocProfileFunc("reputation", (void *)&reputationPerfStats, 0, _dpd.totalPerfStats);
#endif

    }

    sfPolicyUserPolicySet (reputation_config, policy_id);
    pDefaultPolicyConfig = (ReputationConfig *)sfPolicyUserDataGetDefault(reputation_config);
    pPolicyConfig = (ReputationConfig *)sfPolicyUserDataGetCurrent(reputation_config);

    if ((policy_id != 0) && (pDefaultPolicyConfig == NULL))
    {
        DynamicPreprocessorFatalMessage("%s(%d) => Reputation configuration may only"
                " be enabled in default configuration\n",
                *_dpd.config_file, *_dpd.config_line);
    }

    if (pPolicyConfig != NULL)
    {
        DynamicPreprocessorFatalMessage("%s(%d) => Reputation preprocessor can only be "
                "configured once.\n",  *_dpd.config_file, *_dpd.config_line);
    }

    pPolicyConfig = (ReputationConfig *)calloc(1, sizeof(ReputationConfig));
    if (!pPolicyConfig)
    {
        DynamicPreprocessorFatalMessage("Could not allocate memory for "
                "Reputation preprocessor configuration.\n");
    }

    sfPolicyUserDataSetCurrent(reputation_config, pPolicyConfig);

    ParseReputationArgs(pPolicyConfig, (u_char *)argp);

    if ((0 == pPolicyConfig->numEntries)&&(!pPolicyConfig->sharedMem.path))
    {
        return;
    }

    if (policy_id != 0)
        pPolicyConfig->memcap = pDefaultPolicyConfig->memcap;

    if (!pPolicyConfig->sharedMem.path && pPolicyConfig->localSegment)
        IPtables = &pPolicyConfig->localSegment;

    _dpd.addPreproc( sc, ReputationMain, PRIORITY_FIRST, PP_REPUTATION, PROTO_BIT__IP );
#ifdef SHARED_REP
    if (pPolicyConfig->sharedMem.path)
        _dpd.addPostConfigFunc(sc, initShareMemory, pPolicyConfig);
#endif

}

#ifdef REG_TEST
/* Generate zones from ports for regression tests*/
static inline void createZones(uint32_t *ingressZone, uint32_t *egressZone, SFSnortPacket *p)
{
    const uint32_t zone_base = 0xF700;
    *ingressZone = p->src_port - zone_base;
    *egressZone = p->dst_port - zone_base;
}
#endif
/*********************************************************************
 * Lookup the IP information stored in the data entry.
 *
 * Returns:
 *  IPdecision -
 *          DECISION_NULL
 *          BLACKLISTED
 *          WHITELISTED_UNBLACK
 *          MONITORED
 *          WHITELISTED_TRUST
 *
 *********************************************************************/

static inline IPdecision GetReputation(  IPrepInfo * repInfo,
        SFSnortPacket *p, uint32_t *listid)
{
    IPdecision decision = DECISION_NULL;
    uint8_t *base ;
    ListInfo *listInfo;
#ifdef SHARED_REP
    uint32_t ingressZone = 0;
    uint32_t egressZone = 0;
#ifdef DAQ_PKTHDR_UNKNOWN
    if (p->pkt_header)
    {
        ingressZone = p->pkt_header->ingress_group;
        if (p->pkt_header->egress_index < 0)
            egressZone = ingressZone;
        else
            egressZone = p->pkt_header->egress_group;

#ifdef REG_TEST
        createZones(&ingressZone,&egressZone,p);
#endif
        /*Make sure zone ids are in the support range*/
        if (ingressZone >= MAX_NUM_ZONES)
            ingressZone = 0;
        if (egressZone >= MAX_NUM_ZONES)
            egressZone = 0;
    }
#endif
#endif
    /*Walk through the IPrepInfo lists*/
    base = (uint8_t *) reputation_eval_config->iplist;
    listInfo =  (ListInfo *)(&base[reputation_eval_config->iplist->list_info]);

    while(repInfo)
    {
        int i;
        for(i = 0; i < NUM_INDEX_PER_ENTRY; i++)
        {
            int list_index = repInfo->listIndexes[i];
            if (!list_index)
                break;
            list_index--;
#ifdef SHARED_REP
            DEBUG_WRAP(PrintListInfo (listInfo[list_index].zones, listInfo[list_index].listId););
            /*Check both ingress zone and egress zone*/
            if (listInfo[list_index].zones[ingressZone] || listInfo[list_index].zones[egressZone])
#endif
            {
                if (WHITELISTED_UNBLACK == (IPdecision)listInfo[list_index].listType)
                    return DECISION_NULL;
                if (reputation_eval_config->priority == (IPdecision)listInfo[list_index].listType )
                {
                    *listid = listInfo[list_index].listId;
                    return  ((IPdecision)listInfo[list_index].listType);
                }
                else if ( decision < listInfo[list_index].listType)
                {
                    decision = (IPdecision)listInfo[list_index].listType;
                    *listid = listInfo[list_index].listId;
                }
            }

        }
        if (!repInfo->next) break;
        repInfo = (IPrepInfo *)(&base[repInfo->next]);
    }

    return decision;
}

/*********************************************************************
 * Lookup the iplist table.
 *
 * Arguments:
 *  snort_ip_p  - ip to be searched
 *
 * Returns:
 *
 *   IPrepInfo * - The reputation information in the table
 *
 *********************************************************************/
static inline IPrepInfo*  ReputationLookup(snort_ip_p ip)
{
    IPrepInfo * result;


    DEBUG_WRAP( DebugMessage(DEBUG_REPUTATION, "Lookup address: %s \n",sfip_to_str(ip) ););
    if (!reputation_eval_config->scanlocal)
    {
        if (sfip_is_private(ip) )
        {
            DEBUG_WRAP(DebugMessage(DEBUG_REPUTATION, "Private address\n"););
            return NULL;
        }
    }


    result = (IPrepInfo *) sfrt_flat_dir8x_lookup((void *)ip, reputation_eval_config->iplist );


    return (result);

}

/*********************************************************************
 * Make decision based on ip addresses
 *
 * Arguments:
 *  SFSnortPacket * - pointer to packet structure
 *
 * Returns:
 *  IPdecision -
 *          DECISION_NULL
 *          BLACKLISTED
 *          WHITELISTED_UNBLACK
 *          MONITORED
 *          WHITELISTED_TRUST
 *
 *********************************************************************/
static inline IPdecision ReputationDecision(SFSnortPacket *p)
{
    snort_ip_p ip;
    IPdecision decision;
    IPdecision decision_final = DECISION_NULL;
    IPrepInfo *result;

    /*Check INNER IP, when configured or only one layer*/
    if (( ! p->outer_family )
            ||(INNER == reputation_eval_config->nestedIP)
            ||(BOTH == reputation_eval_config->nestedIP))
    {
        ip = GET_INNER_SRC_IP(((SFSnortPacket *)p));
        result = ReputationLookup(ip);
        if(result)
        {
            DEBUG_WRAP(ReputationPrintRepInfo(result,(uint8_t *) reputation_eval_config->iplist););
            decision = GetReputation(result,p, &p->iplist_id);

            p->iprep_layer = IP_INNER_LAYER;
            p->flags |= FLAG_IPREP_SOURCE_TRIGGERED;
            if ( reputation_eval_config->priority == decision)
                return decision;
            decision_final = decision;
        }

        ip = GET_INNER_DST_IP(((SFSnortPacket *)p));
        result = ReputationLookup(ip);
        if(result)
        {
            DEBUG_WRAP(ReputationPrintRepInfo(result,(uint8_t *) reputation_eval_config->iplist););
            decision = GetReputation(result,p, &p->iplist_id);

            p->iprep_layer = IP_INNER_LAYER;
            p->flags &=~FLAG_IPREP_SOURCE_TRIGGERED;
            if ( reputation_eval_config->priority == decision)
                return decision;
            decision_final = decision;
        }
    }
    /*Check OUTER IP*/
    if (( p->outer_family) &&
            ((OUTER == reputation_eval_config->nestedIP)
                    ||(BOTH == reputation_eval_config->nestedIP)))
    {
        ip = GET_OUTER_SRC_IP(((SFSnortPacket *)p));
        result = ReputationLookup(ip);
        if(result)
        {
            decision = GetReputation(result,p, &p->iplist_id);

            p->iprep_layer = IP_OUTTER_LAYER;
            p->flags |= FLAG_IPREP_SOURCE_TRIGGERED;
            if ( reputation_eval_config->priority == decision)
                return decision;
            decision_final = decision;
        }

        ip = GET_OUTER_DST_IP(((SFSnortPacket *)p));
        result = ReputationLookup(ip);
        if(result)
        {
            decision = GetReputation(result,p, &p->iplist_id);

            p->iprep_layer = IP_OUTTER_LAYER;
            p->flags &=~FLAG_IPREP_SOURCE_TRIGGERED;
            if ( reputation_eval_config->priority == decision)
                return decision;
            decision_final = decision;
        }

    }
    return (decision_final);
}

/*********************************************************************
 * Main entry point for Reputation processing.
 *
 * Arguments:
 *  SFSnortPacket * - pointer to packet structure
 *
 * Returns:
 *  None
 *
 *********************************************************************/
static inline void ReputationProcess(SFSnortPacket *p)
{

    IPdecision decision;

    reputation_eval_config->iplist = (table_flat_t *)*IPtables;
    decision = ReputationDecision(p);

    if (DECISION_NULL == decision)
    {
        return;
    }
    else if (BLACKLISTED == decision)
    {
        ALERT(REPUTATION_EVENT_BLACKLIST,REPUTATION_EVENT_BLACKLIST_STR);
#ifdef POLICY_BY_ID_ONLY
        _dpd.inlineForceDropPacket(p);
#endif
        _dpd.disableAllDetect(p);
        _dpd.setPreprocBit(p, PP_PERFMONITOR);
        reputation_stats.blacklisted++;
    }
    else if (MONITORED == decision)
    {
        ALERT(REPUTATION_EVENT_MONITOR,REPUTATION_EVENT_MONITOR_STR);
        p->flags |= FLAG_IPREP_DATA_SET;
        reputation_stats.monitored++;
    }
    else if (WHITELISTED_TRUST == decision)
    {
        ALERT(REPUTATION_EVENT_WHITELIST,REPUTATION_EVENT_WHITELIST_STR);
        p->flags |= FLAG_IGNORE_PORT;
        _dpd.disableAllDetect(p);
        _dpd.setPreprocBit(p, PP_PERFMONITOR);
        reputation_stats.whitelisted++;
    }


}
/* Main runtime entry point for Reputation preprocessor.
 * Analyzes Reputation packets for anomalies/exploits.
 *
 * PARAMETERS:
 *
 * packetp:    Pointer to current packet to process.
 * contextp:   Pointer to context block, not used.
 *
 * RETURNS:     Nothing.
 */
static void ReputationMain( void* ipacketp, void* contextp )
{
    PROFILE_VARS;
    DEBUG_WRAP(DebugMessage(DEBUG_REPUTATION, "%s\n", REPUTATION_DEBUG__START_MSG));

    // preconditions - what we registered for
    assert(IsIP((SFSnortPacket*)ipacketp));

    if (
        ((SFSnortPacket*)ipacketp)->flags & FLAG_REBUILT_FRAG ||
        ((SFSnortPacket*)ipacketp)->flags & FLAG_REBUILT_STREAM )
    {
        DEBUG_WRAP(DebugMessage(DEBUG_REPUTATION,"   -> spp_reputation: Not IP or Is a rebuilt packet\n"););
        DEBUG_WRAP(DebugMessage(DEBUG_REPUTATION, "%s\n", REPUTATION_DEBUG__END_MSG));
        return;
    }

    reputation_eval_config = sfPolicyUserDataGetDefault(reputation_config);

    PREPROC_PROFILE_START(reputationPerfStats);
    ReputationProcess((SFSnortPacket*) ipacketp);
    DEBUG_WRAP(DebugMessage(DEBUG_REPUTATION, "%s\n", REPUTATION_DEBUG__END_MSG));

    PREPROC_PROFILE_END(reputationPerfStats);
}

static int ReputationCheckPolicyConfig(
    struct _SnortConfig *sc, tSfPolicyUserContextId config, tSfPolicyId policyId, void* pData)
{
    _dpd.setParserPolicy(sc, policyId);

    return 0;
}

int ReputationCheckConfig(struct _SnortConfig *sc)
{
    int rval;

    if ((rval = sfPolicyUserDataIterate (sc, reputation_config, ReputationCheckPolicyConfig)))
        return rval;

    return 0;
}

static void ReputationCleanExit(int signal, void *data)
{
    if (reputation_config != NULL)
    {
        ReputationFreeConfig(reputation_config);
        reputation_config = NULL;
#ifdef SHARED_REP
        ShutdownSharedMemory();
        if (emptyIPtables != NULL)
        {
            free(emptyIPtables);
            emptyIPtables = NULL;
        }
#endif
    }
}

static int ReputationFreeConfigPolicy(
        tSfPolicyUserContextId config,
        tSfPolicyId policyId,
        void* pData
)
{
    ReputationConfig *pPolicyConfig = (ReputationConfig *)pData;

    //do any housekeeping before freeing ReputationConfig

    sfPolicyUserDataClear (config, policyId);

    Reputation_FreeConfig(pPolicyConfig);
    return 0;
}

void ReputationFreeConfig(tSfPolicyUserContextId config)
{
    if (config == NULL)
        return;

    sfPolicyUserDataFreeIterate (config, ReputationFreeConfigPolicy);
    sfPolicyConfigDelete(config);
}
/******************************************************************
 * Print statistics being kept by the preprocessor.
 *
 * Arguments:
 *  int - whether Snort is exiting or not
 *
 * Returns: None
 *
 ******************************************************************/
static void ReputationPrintStats(int exiting)
{

    _dpd.logMsg("Reputation Preprocessor Statistics\n");

    _dpd.logMsg("  Total Memory Allocated: "STDu64"\n", reputation_stats.memoryAllocated);

    if (reputation_stats.blacklisted > 0)
        _dpd.logMsg("  Number of packets blacklisted: "STDu64"\n", reputation_stats.blacklisted);
    if (reputation_stats.whitelisted > 0)
        _dpd.logMsg("  Number of packets whitelisted: "STDu64"\n", reputation_stats.whitelisted);
    if (reputation_stats.monitored > 0)
        _dpd.logMsg("  Number of packets monitored: "STDu64"\n", reputation_stats.monitored);

}

#ifdef SNORT_RELOAD
static void ReputationReload(struct _SnortConfig *sc, char *args, void **new_config)
{
    tSfPolicyUserContextId reputation_swap_config = (tSfPolicyUserContextId)*new_config;
    tSfPolicyId policy_id = _dpd.getParserPolicy(sc);
    ReputationConfig * pPolicyConfig = NULL;
    ReputationConfig *pDefaultPolicyConfig = NULL;

    if (reputation_swap_config == NULL)
    {
        //create a context
        reputation_swap_config = sfPolicyConfigCreate();
        if (reputation_swap_config == NULL)
        {
            DynamicPreprocessorFatalMessage("Failed to allocate memory "
                    "for Reputation config.\n");
        }
        *new_config = (void *)reputation_swap_config;
    }

    sfPolicyUserPolicySet (reputation_swap_config, policy_id);
    pPolicyConfig = (ReputationConfig *)sfPolicyUserDataGetCurrent(reputation_swap_config);
    pDefaultPolicyConfig = (ReputationConfig *)sfPolicyUserDataGetDefault(reputation_config);

    if ((policy_id != 0) && (pDefaultPolicyConfig == NULL))
    {
        DynamicPreprocessorFatalMessage("%s(%d) => Reputation configuration may only"
                " be enabled in default configuration\n",
                *_dpd.config_file, *_dpd.config_line);
    }

    if (pPolicyConfig != NULL)
    {
        DynamicPreprocessorFatalMessage("%s(%d) => Reputation preprocessor can only be "
                "configured once.\n",  *_dpd.config_file, *_dpd.config_line);
    }

    pPolicyConfig = (ReputationConfig *)calloc(1, sizeof(ReputationConfig));
    if (!pPolicyConfig)
    {
        DynamicPreprocessorFatalMessage("Could not allocate memory for "
                "Reputation preprocessor configuration.\n");
    }
    sfPolicyUserDataSetCurrent(reputation_swap_config, pPolicyConfig);

    ParseReputationArgs(pPolicyConfig, (u_char *)args);

    if ((0 == pPolicyConfig->numEntries) &&(!pPolicyConfig->sharedMem.path))
    {
        return;
    }
    if ((policy_id != 0) &&(pDefaultPolicyConfig))
        pPolicyConfig->memcap = pDefaultPolicyConfig->memcap;

    _dpd.addPreproc( sc, ReputationMain, PRIORITY_FIRST, PP_REPUTATION, PROTO_BIT__IP );
}

static int ReputationReloadVerify(struct _SnortConfig *sc, void *swap_config)
{
    tSfPolicyUserContextId reputation_swap_config = (tSfPolicyUserContextId)swap_config;
    ReputationConfig * pPolicyConfig = NULL;
    ReputationConfig * pCurrentConfig = NULL;

    if (reputation_swap_config == NULL)
        return 0;

    pPolicyConfig = (ReputationConfig *)sfPolicyUserDataGet(reputation_swap_config, _dpd.getDefaultPolicy());

    if (!pPolicyConfig)
        return 0;


    if (reputation_config != NULL)
    {
        pCurrentConfig = (ReputationConfig *)sfPolicyUserDataGet(reputation_config, _dpd.getDefaultPolicy());
    }

    if (!pCurrentConfig)
        return 0;

    if (pPolicyConfig->memcap != pCurrentConfig->memcap)
    {
        _dpd.errMsg("Reputation reload: Changing memcap settings requires a restart.\n");
        return -1;
    }

#ifdef SHARED_REP
    /* Shared memory is used*/
    if (pPolicyConfig->sharedMem.path || pCurrentConfig->sharedMem.path)
    {
        /*Shared memory setting is changed*/
        if ( (!pCurrentConfig->sharedMem.path)||(!pPolicyConfig->sharedMem.path)
                || strcmp(pPolicyConfig->sharedMem.path, pCurrentConfig->sharedMem.path)
                ||(pPolicyConfig->sharedMem.updateInterval != pCurrentConfig->sharedMem.updateInterval))
        {
            _dpd.errMsg("Reputation reload: Changing memory settings requires a restart.\n");
            return -1;
        }

    }
#endif
    return 0;
}

static int ReputationFreeUnusedConfigPolicy(
        tSfPolicyUserContextId config,
        tSfPolicyId policyId,
        void* pData
)
{
    ReputationConfig *pPolicyConfig = (ReputationConfig *)pData;

    //do any housekeeping before freeing ReputationConfig
    if (pPolicyConfig->ref_count == 0)
    {
        sfPolicyUserDataClear (config, policyId);
        Reputation_FreeConfig(pPolicyConfig);
    }
    return 0;
}

static void * ReputationReloadSwap(struct _SnortConfig *sc, void *swap_config)
{
    tSfPolicyUserContextId reputation_swap_config = (tSfPolicyUserContextId)swap_config;
    tSfPolicyUserContextId old_config = reputation_config;
    ReputationConfig *pDefaultPolicyConfig = NULL;

    if (reputation_swap_config == NULL)
        return NULL;

    reputation_config = reputation_swap_config;

    pDefaultPolicyConfig = (ReputationConfig *)sfPolicyUserDataGetDefault(reputation_config);
    if (pDefaultPolicyConfig->localSegment)
        IPtables = &pDefaultPolicyConfig->localSegment;

    sfPolicyUserDataFreeIterate (old_config, ReputationFreeUnusedConfigPolicy);

    if (sfPolicyUserPolicyGetActive(old_config) == 0)
    {
        /* No more outstanding configs - free the config array */
        return (void *)old_config;
    }

    return NULL;
}

static void ReputationReloadSwapFree(void *data)
{
    if (data == NULL)
        return;

    ReputationFreeConfig((tSfPolicyUserContextId)data);
}
#endif
