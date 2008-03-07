/*
 * Copyright IBM Corp. 2007
 *
 * Authors:
 *  Dan Smith <danms@us.ibm.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmpidt.h"
#include "cmpift.h"
#include "cmpimacs.h"

#include <libcmpiutil/libcmpiutil.h>
#include <libcmpiutil/std_association.h>
#include "misc_util.h"
#include "cs_util.h"
#include "device_parsing.h"

#include "Virt_DevicePool.h"
#include "Virt_RASD.h"

const static CMPIBroker *_BROKER;

static CMPIStatus rasd_to_pool(const CMPIObjectPath *ref,
                               struct std_assoc_info *info,
                               struct inst_list *list)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        uint16_t type;
        const char *id = NULL;
        char *poolid = NULL;
        CMPIInstance *pool = NULL;
        CMPIInstance *inst = NULL;

        if (!match_hypervisor_prefix(ref, info))
                return s;

        if (rasd_type_from_classname(CLASSNAME(ref), &type) != CMPI_RC_OK) {
                cu_statusf(_BROKER, &s,
                           CMPI_RC_ERR_FAILED,
                           "Unable to determine RASD type");
                goto out;
        }

        if (cu_get_str_path(ref, "InstanceID", &id) != CMPI_RC_OK) {
                cu_statusf(_BROKER, &s,
                           CMPI_RC_ERR_FAILED,
                           "Missing InstanceID");
                goto out;
        }

        s = get_rasd_by_name(_BROKER, ref, id, type, NULL, &inst);
        if (s.rc != CMPI_RC_OK)
                goto out;

        poolid = pool_member_of(_BROKER, CLASSNAME(ref), type, id);
        if (poolid == NULL) {
                cu_statusf(_BROKER, &s,
                           CMPI_RC_ERR_FAILED,
                           "Unable to determine pool of `%s'", id);
                goto out;
        }

        s = get_pool_by_name(_BROKER,
                             ref,
                             poolid,
                             &pool);
        if (s.rc != CMPI_RC_OK)
                goto out;

        inst_list_add(list, pool);

 out:
        free(poolid);

        return s;
}

static int filter_by_pool(struct inst_list *dest,
                          struct inst_list *src,
                          const char *_poolid)
{
        int i;
        uint16_t type;
        const char *rasd_id = NULL;
        char *poolid = NULL;

        for (i = 0; i < src->cur; i++) {
                CMPIInstance *inst = src->list[i];
                CMPIObjectPath *op;

                op = CMGetObjectPath(inst, NULL);
                if (op == NULL)
                        continue;

                if (rasd_type_from_classname(CLASSNAME(op), &type) !=
                    CMPI_RC_OK)
                        continue;

                cu_get_str_prop(inst, "InstanceID", &rasd_id);

                poolid = pool_member_of(_BROKER, CLASSNAME(op), type, rasd_id);
                if (STREQ(poolid, _poolid))
                        inst_list_add(dest, inst);
        }

        return dest->cur;
}

static int rasds_from_pool(uint16_t type,
                           const CMPIObjectPath *ref,
                           const char *poolid,
                           const char **properties,
                           struct inst_list *list)
{
        CMPIStatus s;
        virConnectPtr conn = NULL;
        virDomainPtr *doms = NULL;
        int count;
        int i;

        conn = connect_by_classname(_BROKER, CLASSNAME(ref), &s);
        if (conn == NULL)
                return 0;

        count = get_domain_list(conn, &doms);

        for (i = 0; i < count; i++) {
                const char *name;
                struct inst_list tmp;

                inst_list_init(&tmp);

                name = virDomainGetName(doms[i]);

                rasds_for_domain(_BROKER,
                                 name,
                                 type,
                                 ref,
                                 properties,
                                 &tmp);

                filter_by_pool(list, &tmp, poolid);

                inst_list_free(&tmp);

                virDomainFree(doms[i]);
        }

        free(doms);
        virConnectClose(conn);

        return count;
}

static CMPIStatus pool_to_rasd(const CMPIObjectPath *ref,
                               struct std_assoc_info *info,
                               struct inst_list *list)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        const char *poolid;
        uint16_t type;
        CMPIInstance *inst;

        if (!match_hypervisor_prefix(ref, info))
                goto out;

        s = get_pool_by_ref(_BROKER, ref, &inst);
        if (s.rc != CMPI_RC_OK)
                goto out;

        if (cu_get_str_path(ref, "InstanceID", &poolid) != CMPI_RC_OK) {
                cu_statusf(_BROKER, &s,
                           CMPI_RC_ERR_FAILED,
                           "Missing InstanceID");
                goto out;
        }

        type = res_type_from_pool_id(poolid);
        if (type == CIM_RES_TYPE_UNKNOWN) {
                cu_statusf(_BROKER, &s,
                           CMPI_RC_ERR_FAILED,
                           "Invalid InstanceID or unsupported pool type");
                goto out;
        }

        rasds_from_pool(type, 
                        ref,
                        poolid,
                        info->properties,
                        list);

 out:
        return s;
}

LIBVIRT_CIM_DEFAULT_MAKEREF()

static char* antecedent[] = {
        "Xen_ProcessorPool",
        "Xen_MemoryPool",
        "Xen_NetworkPool",
        "Xen_DiskPool",
        "KVM_ProcessorPool",
        "KVM_MemoryPool",
        "KVM_NetworkPool",
        "KVM_DiskPool",
        NULL
};

static char* dependent[] = {
        "Xen_DiskResourceAllocationSettingData",
        "Xen_MemResourceAllocationSettingData",
        "Xen_NetResourceAllocationSettingData",
        "Xen_ProcResourceAllocationSettingData",
        "KVM_DiskResourceAllocationSettingData",
        "KVM_MemResourceAllocationSettingData",
        "KVM_NetResourceAllocationSettingData",
        "KVM_ProcResourceAllocationSettingData",
        NULL
};

static char* assoc_classname[] = {
        "Xen_ResourceAllocationFromPool",
        "KVM_ResourceAllocationFromPool",        
        NULL
};

static struct std_assoc _rasd_to_pool = {
        .source_class = (char**)&dependent,
        .source_prop = "Dependent",
        
        .target_class = (char**)&antecedent,
        .target_prop = "Antecedent",

        .assoc_class = (char**)&assoc_classname,

        .handler = rasd_to_pool,
        .make_ref = make_ref
};

static struct std_assoc _pool_to_rasd = {
        .source_class = (char**)&antecedent,
        .source_prop = "Antecedent",

        .target_class = (char**)&dependent,
        .target_prop = "Dependent",

        .assoc_class = (char**)&assoc_classname,

        .handler = pool_to_rasd,
        .make_ref = make_ref
};

static struct std_assoc *handlers[] = {
        &_rasd_to_pool,
        &_pool_to_rasd,
        NULL
};

STDA_AssocMIStub(, 
                 Virt_ResourceAllocationFromPool,
                 _BROKER,
                 libvirt_cim_init(),
                 handlers);

/*
 * Local Variables:
 * mode: C
 * c-set-style: "K&R"
 * tab-width: 8
 * c-basic-offset: 8
 * indent-tabs-mode: nil
 * End:
 */
