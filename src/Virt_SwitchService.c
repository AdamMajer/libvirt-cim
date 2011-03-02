/*
 * Copyright IBM Corp. 2011
 *
 * Authors:
 *  Sharad Mishra <snmishra@us.ibm.com>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include <cmpidt.h>
#include <cmpift.h>
#include <cmpimacs.h>

#include <libcmpiutil/libcmpiutil.h>
#include <libcmpiutil/std_instance.h>

#include "misc_util.h"
#include "config.h"

#define MAX_LEN 512
#define CMD "/sbin/ifconfig -a | /bin/grep eth | /bin/awk '{print$1}'"

const static CMPIBroker *_BROKER;

static CMPIStatus check_vsi_support(char *command)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        char buff[MAX_LEN];
        FILE *stream = NULL;
        const char *searchStr[] = {"	supported forwarding mode: "
                                   "(0x40) reflective relay",
                                   "	supported capabilities: "
                                   "(0x07) RTE ECP VDP"};
        int  matched = 0;

        // Run lldptool command to find vsi support.
        stream = popen(command, "r");
        if (stream == NULL) {
                CU_DEBUG("Failed to open pipe to read vsi support");
                cu_statusf(_BROKER, &s,
                           CMPI_RC_ERR_NOT_FOUND,
                           "Failed to open pipe");
                goto out;
        }

        // Read the output of the command.
        while (fgets(buff, MAX_LEN, stream) != NULL) {
                int i = 0;
                while (searchStr[i] != NULL) {
                        int len = strlen(searchStr[i]);
                        /* Read value which is stored in 'buff' has newline
                           at the end, we remove it for comparison. */
                        if (strncmp(buff, searchStr[i], (len - 1)) == 0) {
                                /* 'matched' flag is incremented each time
                                   we find that read string (output of lldptool
                                   command) and searchStrings are same. */
                                matched++;
                                break;
                        }
                        i++;
                }
                /* All the search strings were found in the output of this
                   command. */
                if (matched == 2) {
                        cu_statusf(_BROKER, &s, CMPI_RC_OK, "VSI supported");
                        goto out;;
                }
        }
        cu_statusf(_BROKER, &s,
                   CMPI_RC_ERR_NOT_FOUND,
                   "No VSI Support found");

 out:       
        pclose(stream);
        return s;
}

static char **run_command(char *func, int *len, CMPIStatus *s) {

        char buff[MAX_LEN];
        FILE *stream = NULL;
        char **arr = NULL;
        char *string = NULL;
        int i = 0;

        // run the command.
        stream = popen(func, "r");
        if (stream == NULL) {
                CU_DEBUG("Failed to open pipe to run command");
                cu_statusf(_BROKER, s,
                           CMPI_RC_ERR_NOT_FOUND,
                           "Failed to open pipe");
                return NULL;
        }

        // read output of command.
        while (fgets(buff, MAX_LEN, stream) != NULL) {
                int len = strlen(buff) - 1;
                char **tmp_list = NULL;

                // dynamically increase size as more interfaces are found.
                tmp_list = (char **)realloc(arr,
                                            (i + 1) *
                                            sizeof(char *));
                if (tmp_list == NULL) {
                        CU_DEBUG("Failed to allocate memory");
                        cu_statusf(_BROKER, s,
                                   CMPI_RC_ERR_NOT_FOUND,
                                   "Failed to realloc");
                        return NULL;
                }

                arr = tmp_list;

                string = calloc(len, sizeof(char));
                if (string == NULL) {
                        CU_DEBUG("Failed to allocate memory");
                        cu_statusf(_BROKER, s,
                                   CMPI_RC_ERR_NOT_FOUND,
                                   "Failed to calloc");
                        return NULL;
                }
                strncpy(string,  buff, len);
                arr[i] = string;
                i++;
        }

        pclose(stream);
        *len = i;
        return arr;
}

static CMPIStatus get_switchservice(const CMPIObjectPath *reference,
                         CMPIInstance **_inst,
                         const CMPIBroker *broker,
                         const CMPIContext *context,
                         bool is_get_inst)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        CMPIInstance *inst = NULL;
        virConnectPtr conn = NULL;
        bool vsi = false;
        int count = 0;
        int i;
        char **if_list;
        char cmd[MAX_LEN];

        *_inst = NULL;
        conn = connect_by_classname(broker, CLASSNAME(reference), &s);
        if (conn == NULL) {
                if (is_get_inst)
                        cu_statusf(broker, &s,
                                   CMPI_RC_ERR_NOT_FOUND,
                                   "No such instance");

                return s;
        }

        inst = get_typed_instance(broker,
                                  pfx_from_conn(conn),
                                  "SwitchService",
                                  NAMESPACE(reference));

        if (inst == NULL) {
                CU_DEBUG("Failed to get typed instance");
                cu_statusf(broker, &s,
                           CMPI_RC_ERR_FAILED,
                           "Failed to create instance");
                goto out;
        }

        CMSetProperty(inst, "Name",
                      (CMPIValue *)"Switch Virtualization Capabilities", 
                      CMPI_chars);
    
        if_list = run_command(CMD, &count, &s);
        if (if_list == 0) {
                CU_DEBUG("Failed to get network interfaces");
                cu_statusf(broker, &s,
                           CMPI_RC_ERR_FAILED,
                           "Failed to get network interfaces");
                goto out;
        }

        CU_DEBUG("Found %d interfaces", count);


        for (i=0; i<count; i++) {
                sprintf(cmd, "lldptool -i %s -t -V evbcfg", if_list[i]);
                CU_DEBUG("running command %s ...", cmd);
                s = check_vsi_support(cmd); 
                if (s.rc == CMPI_RC_OK) {
                        vsi = true;
                        break;
                }
                else
                        vsi = false;
        }

        CMSetProperty(inst, "IsVSISupported", (CMPIValue *)&vsi, CMPI_boolean);
        s.rc = CMPI_RC_OK;

 out:
        virConnectClose(conn);
        *_inst = inst;

        return s;

}

static CMPIStatus return_switchservice(const CMPIContext *context,
                            const CMPIObjectPath *reference,
                            const CMPIResult *results,
                            bool name_only,
                            bool is_get_inst)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        CMPIInstance *inst;

        s = get_switchservice(reference, &inst, _BROKER, context, is_get_inst);
        if (s.rc != CMPI_RC_OK || inst == NULL)
                goto out;

        if (name_only)
                cu_return_instance_name(results, inst);
        else
                CMReturnInstance(results, inst);
 out:
        return s;
}

static CMPIStatus EnumInstanceNames(CMPIInstanceMI *self,
                                    const CMPIContext *context,
                                    const CMPIResult *results,
                                    const CMPIObjectPath *ref)
{
        return return_switchservice(context, ref, results, true, false);
}

static CMPIStatus EnumInstances(CMPIInstanceMI *self,
                                const CMPIContext *context,
                                const CMPIResult *results,
                                const CMPIObjectPath *ref,
                                const char **properties)
{

        return return_switchservice(context, ref, results, false, false);
}

static CMPIStatus GetInstance(CMPIInstanceMI *self,
                              const CMPIContext *context,
                              const CMPIResult *results,
                              const CMPIObjectPath *ref,
                              const char **properties)
{
        return return_switchservice(context, ref, results, false, true);
}

DEFAULT_CI();
DEFAULT_MI();
DEFAULT_DI();
DEFAULT_EQ();
DEFAULT_INST_CLEANUP();

STD_InstanceMIStub(,
                   Virt_SwitchService,
                   _BROKER,
                   libvirt_cim_init());

/*
 * Local Variables:
 * mode: C
 * c-set-style: "K&R"
 * tab-width: 8
 * c-basic-offset: 8
 * indent-tabs-mode: nil
 * End:
 */

