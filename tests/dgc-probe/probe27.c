/*
 * probe27 - A7xx inline CP_COND_EXEC chunk guard correctness.
 * Exercises count<<max across multiple chunks; implicit and explicit-preprocess.
 */
#include "pcommon.h"
#include "vs_full_spv.h"
#include "fs_green_spv.h"

static PFN_vkCreateIndirectCommandsLayoutEXT pCreateLayout;
static PFN_vkGetGeneratedCommandsMemoryRequirementsEXT pMemReq;
static PFN_vkCmdExecuteGeneratedCommandsEXT pExecute;

#define MAX_SEQ 4096
struct seq_draw { uint32_t vertex_count, instance_count, first_vertex, first_instance; };

static void run_case(struct env *e, uint32_t count, uint32_t want, VkBool32 is_preprocessed)
{
   struct stagex st[2]={{VK_SHADER_STAGE_VERTEX_BIT,VS_FULL_SPV,VS_FULL_SPV_N*4},{VK_SHADER_STAGE_FRAGMENT_BIT,FS_GREEN_SPV,FS_GREEN_SPV_N*4}};
   VkPipelineLayout pl; VkPipeline pipe=make_pipeline_ex(e,st,2,NULL,0,NULL,&pl);
   VkIndirectCommandsLayoutTokenEXT tok={.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,.type=VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT,.offset=0};
   VkIndirectCommandsLayoutCreateInfoEXT lci={.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,
      .flags = is_preprocessed ? VK_INDIRECT_COMMANDS_LAYOUT_USAGE_EXPLICIT_PREPROCESS_BIT_EXT : 0,
      .shaderStages=VK_SHADER_STAGE_ALL_GRAPHICS,.indirectStride=sizeof(struct seq_draw),.pipelineLayout=pl,.tokenCount=1,.pTokens=&tok};
   VkIndirectCommandsLayoutEXT lay; VKCHECK(pCreateLayout(e->dev,&lci,NULL,&lay));

   struct seq_draw *seqs=calloc(MAX_SEQ,sizeof(*seqs));
   for(int i=0;i<MAX_SEQ;i++) seqs[i]=(struct seq_draw){3,1,0,0};
   VkBuffer sb; VkDeviceMemory sm; VkDeviceAddress sva=bda_usage_buffer(e,&sb,&sm,(VkDeviceSize)MAX_SEQ*sizeof(*seqs),VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
   buf_write(e,sm,seqs,(VkDeviceSize)MAX_SEQ*sizeof(*seqs)); free(seqs);

   uint32_t cnt=count; VkBuffer cb; VkDeviceMemory cm; VkDeviceAddress cva=bda_usage_buffer(e,&cb,&cm,sizeof(cnt),VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
   buf_write(e,cm,&cnt,sizeof(cnt));

   VkBuffer ppb; VkDeviceMemory ppm; VkDeviceSize ppsz=0; VkDeviceAddress ppva=preprocess_buffer(e,pMemReq,lay,pipe,MAX_SEQ,&ppsz,&ppb,&ppm);
   VkGeneratedCommandsPipelineInfoEXT pi={.sType=VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,.pipeline=pipe};
   VkGeneratedCommandsInfoEXT gi={.sType=VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,.pNext=&pi,.shaderStages=VK_SHADER_STAGE_ALL_GRAPHICS,.indirectCommandsLayout=lay,
      .indirectAddress=sva,.indirectAddressSize=(VkDeviceSize)MAX_SEQ*sizeof(*seqs),.preprocessAddress=ppva,.preprocessSize=ppsz,.sequenceCountAddress=cva,.maxSequenceCount=MAX_SEQ,.maxDrawCount=MAX_SEQ};

   VkCommandBufferBeginInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   VKCHECK(vkBeginCommandBuffer(e->cmd,&cbi));
   vkCmdBindPipeline(e->cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
   begin_renderpass(e,C_MAGENTA);
   VkDeviceSize zero=0; vkCmdBindVertexBuffers(e->cmd,0,1,&e->vbo,&zero);
   if (is_preprocessed) {
      pExecute(e->cmd,VK_TRUE,&gi);
      end_renderpass(e);
      VKCHECK(vkEndCommandBuffer(e->cmd));
      VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&e->cmd};
      VKCHECK(vkQueueSubmit(e->queue,1,&si,e->fence)); VKCHECK(vkWaitForFences(e->dev,1,&e->fence,VK_TRUE,UINT64_MAX));
      VKCHECK(vkResetFences(e->dev,1,&e->fence)); VKCHECK(vkResetCommandPool(e->dev,e->pool,0));

      VKCHECK(vkBeginCommandBuffer(e->cmd,&cbi));
      vkCmdBindPipeline(e->cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
      begin_renderpass(e,C_MAGENTA);
      vkCmdBindVertexBuffers(e->cmd,0,1,&e->vbo,&zero);
      pExecute(e->cmd,VK_FALSE,&gi);
      end_renderpass(e); VKCHECK(vkEndCommandBuffer(e->cmd));
   } else {
      pExecute(e->cmd,VK_FALSE,&gi);
      end_renderpass(e); VKCHECK(vkEndCommandBuffer(e->cmd));
   }
   VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&e->cmd};
   VKCHECK(vkQueueSubmit(e->queue,1,&si,e->fence)); VKCHECK(vkWaitForFences(e->dev,1,&e->fence,VK_TRUE,UINT64_MAX));
   VKCHECK(vkResetFences(e->dev,1,&e->fence)); VKCHECK(vkResetCommandPool(e->dev,e->pool,0));
   void *map=readback(e);
   char what[80]; snprintf(what,sizeof(what),"count=%u/%s",count,is_preprocessed?"pre":"imp");
   check(e,what,W/2,H/2,px(map,W/2,H/2),want);

   PFN_vkDestroyIndirectCommandsLayoutEXT pDestroyLayout=(PFN_vkDestroyIndirectCommandsLayoutEXT)vkGetDeviceProcAddr(e->dev,"vkDestroyIndirectCommandsLayoutEXT");
   vkDestroyPipeline(e->dev,pipe,NULL); vkDestroyPipelineLayout(e->dev,pl,NULL);
   if(pDestroyLayout) pDestroyLayout(e->dev,lay,NULL);
   vkDestroyBuffer(e->dev,sb,NULL); vkFreeMemory(e->dev,sm,NULL);
   vkDestroyBuffer(e->dev,cb,NULL); vkFreeMemory(e->dev,cm,NULL);
   vkDestroyBuffer(e->dev,ppb,NULL); vkFreeMemory(e->dev,ppm,NULL);
}

int main(void)
{
   struct env e; struct env_ext ext={.dgc=VK_TRUE}; env_init_ex(&e,"probe27",&ext);
   pCreateLayout=(PFN_vkCreateIndirectCommandsLayoutEXT)vkGetDeviceProcAddr(e.dev,"vkCreateIndirectCommandsLayoutEXT");
   pMemReq=(PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)vkGetDeviceProcAddr(e.dev,"vkGetGeneratedCommandsMemoryRequirementsEXT");
   pExecute=(PFN_vkCmdExecuteGeneratedCommandsEXT)vkGetDeviceProcAddr(e.dev,"vkCmdExecuteGeneratedCommandsEXT");
   if(!pCreateLayout||!pMemReq||!pExecute) return 3;
   float verts[3][2]={{-0.008f,0.008f},{0.008f,0.008f},{0.0f,-0.016f}};
   buf_create(&e,&e.vbo,&e.vmem,sizeof(verts),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,1); buf_write(&e,e.vmem,verts,sizeof(verts));

   uint32_t counts[] = {0,1,2,63,64,65,255,256,1024,MAX_SEQ-1,MAX_SEQ};
   for (unsigned i=0;i<sizeof(counts)/sizeof(counts[0]);i++) {
      uint32_t c=counts[i], want=c?C_GREEN:C_MAGENTA;
      run_case(&e,c,want,VK_FALSE);
      run_case(&e,c,want,VK_TRUE);
   }
   printf("probe27: %s (%d checks, %d fail)\n", e.fails?"FAIL":"ok", e.checks, e.fails);
   return e.fails?1:0;
}
