/*
 * probe16 — последовательность [PUSH_CONSTANT, DISPATCH], как её просит
 * vkd3d для Starfield: в одной последовательности и значение push-константы,
 * и грид.
 *
 * Данные последовательности: { magic, scale, gridX, gridY, gridZ },
 * indirectStride = 20. До execute кладём в push constants заведомо другие
 * значения, чтобы «токен проигнорирован» отличалось от «токен применён».
 *
 * Ядро пишет v[i] = magic + scale * i (i — номер инвокации) и маркеры:
 *   [8]  0xdeadbeef        — сетка вообще запустилась;
 *   [9]  gl_NumWorkGroups.x — грид из DISPATCH-токена доехал до шейдера;
 *   [10] gl_NumWorkGroups.y — и по Y тоже.
 *
 * RC: 0 = всё сошлось, 3 = нет точек входа, 4 = сетка не запустилась,
 * 5 = gl_NumWorkGroups не тот, 6 = push-константа не из токена.
 */
#include "pcommon.h"
#include "cs_dgc_pc_spv.h"

#define MAGIC 0x5a5a0001u
#define SCALE 0x100u
#define WRONG_MAGIC 0x1111u
#define WRONG_SCALE 0x2222u
#define GRID_X 3u

static PFN_vkCreateIndirectCommandsLayoutEXT pCreateLayout;
static PFN_vkGetGeneratedCommandsMemoryRequirementsEXT pMemReq;
static PFN_vkCmdExecuteGeneratedCommandsEXT pExecute;

static void
load_dgc(struct env *e)
{
   pCreateLayout = (PFN_vkCreateIndirectCommandsLayoutEXT)
      vkGetDeviceProcAddr(e->dev, "vkCreateIndirectCommandsLayoutEXT");
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
   env_init_ex(&e, "probe16", &ext);
   load_dgc(&e);
   if (!pCreateLayout || !pMemReq || !pExecute) {
      fprintf(stderr, "probe16: DGC entry points missing\n");
      return 3;
   }

   /* Вывод ядра, host visible, обнулён. */
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

   /* Push constants: ровно те 8 байт, которые трогает токен. */
   VkPushConstantRange pcr = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0,
      .size = 8,
   };
   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &dsl,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pcr,
   };
   VkPipelineLayout pl;
   VKCHECK(vkCreatePipelineLayout(e.dev, &plci, NULL, &pl));

   VkShaderModuleCreateInfo mci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = CS_DGC_PC_SPV_N * 4,
      .pCode = CS_DGC_PC_SPV,
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

   /* Одна последовательность: два dword'а push-константы, три — грид. */
   VkBuffer seqbuf;
   VkDeviceMemory seqmem;
   const uint32_t stride = 5 * sizeof(uint32_t);
   VkDeviceAddress seq_va = bda_buffer(&e, &seqbuf, &seqmem, stride);
   uint32_t seq[5] = { MAGIC, SCALE, GRID_X, 1, 1 };
   buf_write(&e, seqmem, seq, sizeof(seq));

   VkIndirectCommandsLayoutTokenEXT toks[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
         .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT,
         .offset = 0,
         .data.pPushConstant = &(VkIndirectCommandsPushConstantTokenEXT){
            .updateRange = { VK_SHADER_STAGE_COMPUTE_BIT, 0,
                             2 * sizeof(uint32_t) }
         },
      },
      {
         .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
         .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DISPATCH_EXT,
         .offset = 2 * sizeof(uint32_t),
      },
   };
   VkIndirectCommandsLayoutCreateInfoEXT lci = {
      .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,
      .shaderStages = VK_SHADER_STAGE_COMPUTE_BIT,
      .indirectStride = stride,
      .pipelineLayout = pl,
      .tokenCount = 2,
      .pTokens = toks,
   };
   VkIndirectCommandsLayoutEXT lay;
   VkResult lr = pCreateLayout(e.dev, &lci, NULL, &lay);
   if (lr != VK_SUCCESS) {
      fprintf(stderr, "probe16: CreateIndirectCommandsLayoutEXT failed: %d\n",
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
      .indirectAddressSize = stride,
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

   /* Значения, которые токен обязан перезаписать. Если токен не применён,
    * шейдер увидит именно их. */
   const uint32_t wrong[2] = { WRONG_MAGIC, WRONG_SCALE };
   vkCmdPushConstants(e.cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                      sizeof(wrong), wrong);

   pExecute(e.cmd, VK_FALSE, &gi);
   VKCHECK(vkEndCommandBuffer(e.cmd));
   submit_wait(&e);

   /* Дамп сгенерированного стрима — на reset'е буфера (TU_DEBUG=dgcdump). */
   VKCHECK(vkResetCommandBuffer(e.cmd, 0));

   void *map;
   VKCHECK(vkMapMemory(e.dev, smem, 0, 64, 0, &map));
   uint32_t *v = map;
   printf("probe16: v[8]=%#x v[9]=%u (gl_NumWorkGroups.x) v[10]=%u (y)\n",
          v[8], v[9], v[10]);
   printf("probe16: v[0]=%#x v[1]=%#x v[2]=%#x, ожидалось %#x %#x %#x\n",
          v[0], v[1], v[2], MAGIC, MAGIC + SCALE, MAGIC + 2 * SCALE);

   int rc = 0;
   if (v[8] != 0xdeadbeefu) {
      printf("probe16: сетка не запустилась\n");
      rc = 4;
   } else if (v[9] != GRID_X || v[10] != 1) {
      printf("probe16: грид из токена не доехал: %u,%u, ожидалось %u,1\n",
             v[9], v[10], GRID_X);
      rc = 5;
   } else if (v[0] != MAGIC || v[1] != MAGIC + SCALE ||
              v[2] != MAGIC + 2 * SCALE) {
      printf("probe16: push-константа не из токена (%#x — значит токен "
             "проигнорирован)\n", WRONG_MAGIC);
      rc = 6;
   } else {
      printf("probe16: [PUSH_CONSTANT, DISPATCH] работает: значение и грид "
             "из последовательности\n");
   }
   printf("probe16: RC=%d\n", rc);
   return rc;
}
