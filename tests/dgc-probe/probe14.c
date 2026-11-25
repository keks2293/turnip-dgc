/*
 * probe14 — DISPATCH-токен через vkCmdExecuteGeneratedCommandsEXT.
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
   env_init_ex(&e, "probe14", &ext);
   load_dgc(&e);
   if (!pCreateLayout || !pCreateIES || !pMemReq || !pExecute) {
      fprintf(stderr, "probe14: DGC entry points missing\n");
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

   /* One sequence: group counts 2x1x1. */
   VkBuffer seqbuf;
   VkDeviceMemory seqmem;
   VkDeviceAddress seq_va = bda_buffer(&e, &seqbuf, &seqmem, 16);
   VkDispatchIndirectCommand seq = {.x = 2, .y = 1, .z = 1};
   buf_write(&e, seqmem, &seq, sizeof(seq));

   VkIndirectCommandsLayoutTokenEXT tok = {
      .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
      .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DISPATCH_EXT,
      .offset = 0,
   };
   VkIndirectCommandsLayoutCreateInfoEXT lci = {
      .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,
      .shaderStages = VK_SHADER_STAGE_COMPUTE_BIT,
      .indirectStride = 16,
      .pipelineLayout = pl,
      .tokenCount = 1,
      .pTokens = &tok,
   };
   VkIndirectCommandsLayoutEXT lay;
   VkResult lr = pCreateLayout(e.dev, &lci, NULL, &lay);
   if (lr != VK_SUCCESS) {
      fprintf(stderr, "probe14: CreateIndirectCommandsLayoutEXT failed: %d\n",
              lr);
      return 4;
   }

   /* Layout без токена EXECUTION_SET не принимает execution set
    * (VUID-VkGeneratedCommandsInfoEXT-indirectCommandsLayout-10241), поэтому
    * pipeline, для которого строятся последовательности, едет в pNext и
    * запроса требований, и самого execute (-11080). */
   VkGeneratedCommandsPipelineInfoEXT with_pipe = {
      .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,
      .pipeline = pipe,
   };

   VkDeviceSize pp_size;
   VkBuffer ppbuf;
   VkDeviceMemory ppmem;
   VkDeviceAddress pp_va =
      preprocess_buffer(&e, pMemReq, lay, pipe, 1, &pp_size, &ppbuf, &ppmem);

   VkGeneratedCommandsInfoEXT gi = {
      .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,
      .pNext = &with_pipe,
      .shaderStages = VK_SHADER_STAGE_COMPUTE_BIT,
      .indirectCommandsLayout = lay,
      .indirectAddress = seq_va,
      .indirectAddressSize = sizeof(seq),
      .preprocessAddress = pp_va,
      .preprocessSize = pp_size,
      .maxSequenceCount = 1,
      .maxDrawCount = 1,
   };

   VkCommandBufferBeginInfo cbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   VKCHECK(vkBeginCommandBuffer(e.cmd, &cbi));
   vkCmdBindPipeline(e.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(e.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1,
                           &set, 0, NULL);
   pExecute(e.cmd, VK_FALSE, &gi);
   VKCHECK(vkEndCommandBuffer(e.cmd));
   submit_wait(&e);

   /* The driver dumps the generated stream and the per-sequence driver
    * params when the command buffer is reset, which is after the GPU wrote
    * them (TU_DEBUG=dgcdump). */
   VKCHECK(vkResetCommandBuffer(e.cmd, 0));

   void *map;
   VKCHECK(vkMapMemory(e.dev, smem, 0, 64, 0, &map));
   uint32_t *v = map;
   printf("probe14: v[0]=%#x v[1]=%u (gl_NumWorkGroups.x) v[2]=%u "
          "(gl_GlobalInvocationID.x)\n", v[0], v[1], v[2]);

   int rc = 0;
   if (v[0] != 0xdeadbeefu) {
      printf("probe14: grid did NOT run => SP_CS_NDRANGE.GLOBALSIZE is "
             "authoritative, CP_EXEC_CS.NGROUPS alone is not enough\n");
      rc = 4;
   } else if (v[1] != 2) {
      printf("probe14: grid ran, but gl_NumWorkGroups.x=%u, expected 2 "
             "(driver params were not patched)\n", v[1]);
      rc = 5;
   } else {
      printf("probe14: DISPATCH token works: grid from the sequence, "
             "gl_NumWorkGroups patched\n");
   }
   printf("probe14: RC=%d\n", rc);
   return rc;
}
