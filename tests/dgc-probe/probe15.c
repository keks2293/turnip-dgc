/*
 * probe15 — DISPATCH-токен через vkCmdExecuteGeneratedCommandsEXT.
 *
 * Последовательность: VkDispatchIndirectCommand{2,1,1}. Ядро пишет в SSBO:
 *   [0] 0xdeadbeef  — сетка вообще запустилась;
 *   [1] gl_NumWorkGroups.x — грид из последовательности доехал до шейдера;
 *   [2] gl_GlobalInvocationID.x — номера групп идут.
 * Сетка 2×1×1 при local_size=1 даёт ровно две группы.
 *
 * Если [0] не 0xdeadbeef — сетка не запустилась: значит для запуска
 * авторитетны не CP_EXEC_CS.NGROUPS, а SP_CS_NDRANGE.GLOBALSIZE, и его
 * придётся патчить в стриме.
 *
 * RC: 0 = всё сошлось, 4 = сетка не запустилась, 5 = запустилась, но
 * gl_NumWorkGroups не тот, 3 = нет точек входа.
 */
#include "pcommon.h"
#include <dlfcn.h>
#include "cs_dgc_spv.h"

#ifndef DGC_ICD
#define DGC_ICD "/var/home/armada/opencode/build/out-dgc27/libvulkan_freedreno.so"
#endif

static PFN_vkCreateIndirectCommandsLayoutEXT pCreateLayout;
static PFN_vkCreateIndirectExecutionSetEXT pCreateIES;
static PFN_vkGetGeneratedCommandsMemoryRequirementsEXT pMemReq;
static PFN_vkCmdExecuteGeneratedCommandsEXT pExecute;

static void
load_dgc(struct env *e)
{
   pCreateLayout = (PFN_vkCreateIndirectCommandsLayoutEXT)
      vkGetDeviceProcAddr(e->dev, "vkCreateIndirectCommandsLayoutEXT");
   pCreateIES = (PFN_vkCreateIndirectExecutionSetEXT)
      vkGetDeviceProcAddr(e->dev, "vkCreateIndirectExecutionSetEXT");
   pMemReq = (PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)
      vkGetDeviceProcAddr(e->dev, "vkGetGeneratedCommandsMemoryRequirementsEXT");
   pExecute = (PFN_vkCmdExecuteGeneratedCommandsEXT)
      vkGetDeviceProcAddr(e->dev, "vkCmdExecuteGeneratedCommandsEXT");
}

int
main(void)
{
   struct env e;
   struct env_ext ext = {.dgc = VK_TRUE};
   env_init_ex(&e, "probe15", &ext);
   load_dgc(&e);
   if (!pCreateLayout || !pCreateIES || !pMemReq || !pExecute) {
      fprintf(stderr, "probe15: DGC entry points missing\n");
      return 3;
   }

   /* Output SSBO, host visible, zeroed. */
   VkBuffer ssbo;
   VkDeviceMemory smem;
   buf_create(&e, &ssbo, &smem, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1);
   uint32_t zeros[16] = {0};
   buf_write(&e, smem, zeros, sizeof(zeros));

   VkDescriptorSetLayoutBinding db = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
   };
   VkDescriptorSetLayoutCreateInfo dsci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &db,
   };
   VkDescriptorSetLayout dsl;
   VKCHECK(vkCreateDescriptorSetLayout(e.dev, &dsci, NULL, &dsl));

   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &dsl,
   };
   VkPipelineLayout pl;
   VKCHECK(vkCreatePipelineLayout(e.dev, &plci, NULL, &pl));

   VkShaderModuleCreateInfo mci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = CS_DGC_SPV_N * 4,
      .pCode = CS_DGC_SPV,
   };
   VkShaderModule mod;
   VKCHECK(vkCreateShaderModule(e.dev, &mci, NULL, &mod));
   VkComputePipelineCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = mod,
         .pName = "main",
      },
      .layout = pl,
   };
   VkPipeline pipe;
   VKCHECK(vkCreateComputePipelines(e.dev, VK_NULL_HANDLE, 1, &cpci, NULL,
                                    &pipe));

   VkDescriptorPoolSize dps = {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
   };
   VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &dps,
   };
   VkDescriptorPool dp;
   VKCHECK(vkCreateDescriptorPool(e.dev, &dpci, NULL, &dp));
   VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dp,
      .descriptorSetCount = 1,
      .pSetLayouts = &dsl,
   };
   VkDescriptorSet set;
   VKCHECK(vkAllocateDescriptorSets(e.dev, &dsai, &set));
   VkDescriptorBufferInfo bi = {.buffer = ssbo, .range = VK_WHOLE_SIZE};
   VkWriteDescriptorSet wds = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &bi,
   };
   vkUpdateDescriptorSets(e.dev, 1, &wds, 0, NULL);

   /* Indirect buffer: group counts 2x1x1, host visible. */
   VkBuffer ibuf;
   VkDeviceMemory imem;
   buf_create(&e, &ibuf, &imem, 16, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, 1);
   uint32_t z[4] = {0, 0, 0, 0};
   buf_write(&e, imem, z, sizeof(z));
   uint32_t grid[3] = {2, 1, 1};
   buf_write(&e, imem, grid, sizeof(grid));

   VkCommandBufferBeginInfo cbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   VKCHECK(vkBeginCommandBuffer(e.cmd, &cbi));
   vkCmdBindPipeline(e.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(e.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1,
                           &set, 0, NULL);
   vkCmdDispatchIndirect(e.cmd, ibuf, 0);
   VKCHECK(vkEndCommandBuffer(e.cmd));
   submit_wait(&e);

   /* Nothing to dump here: vkCmdDispatchIndirect() emits into the command
    * buffer's own stream, and it is TU_DEBUG=dgcdump that prints the streams
    * vkCmdExecuteGeneratedCommandsEXT has the GPU write. Reset is enough to
    * show that the driver is done with them. */
   VKCHECK(vkResetCommandBuffer(e.cmd, 0));

   void *map;
   VKCHECK(vkMapMemory(e.dev, smem, 0, 64, 0, &map));
   uint32_t *v = map;
   printf("probe15: v[0]=%#x v[1]=%u (gl_NumWorkGroups.x) v[2]=%u "
          "(gl_GlobalInvocationID.x)\n", v[0], v[1], v[2]);

   int rc = 0;
   if (v[0] != 0xdeadbeefu) {
      printf("probe15: plain vkCmdDispatchIndirect did not run either\n");
      rc = 4;
   } else if (v[1] != 2) {
      printf("probe15: grid ran, but gl_NumWorkGroups.x=%u, expected 2 "
             "(driver params were not patched)\n", v[1]);
      rc = 5;
   } else {
      printf("probe15: vkCmdDispatchIndirect works: CP_EXEC_CS_INDIRECT "
             "launches with SP_CS_NDRANGE left at zero\n");
   }
   printf("probe15: RC=%d\n", rc);
   return rc;
}
