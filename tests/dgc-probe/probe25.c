/*
 * probe25 - the stream tail for a dynamic count much smaller than
 * maxSequenceCount.
 *
 * The preprocess shader fills the slots past the count with CP_NOP so the CP
 * can fetch and parse them, and it now covers them with a few large NOP
 * packets instead of one NOP per dword (the dgc_emit_padding approach in
 * radv). A maxSequenceCount large enough to need several chunks of the 14-bit
 * NOP length exercises that loop. The command count itself is tiny or zero.
 */
#include "pcommon.h"
#include "vs_full_spv.h"
#include "fs_green_spv.h"

static PFN_vkCreateIndirectCommandsLayoutEXT pCreateLayout;
static PFN_vkGetGeneratedCommandsMemoryRequirementsEXT pMemReq;
static PFN_vkCmdExecuteGeneratedCommandsEXT pExecute;

/* Large enough that (max - count) * dwords_per_seq crosses the 0x3FFF
 * payload limit of a CP_NOP packet several times. */
#define MAX_SEQ 8192

struct seq_draw { uint32_t vertex_count, instance_count, first_vertex, first_instance; };

static void run_count(struct env *e, uint32_t count, uint32_t want)
{
   struct stagex st[2]={{VK_SHADER_STAGE_VERTEX_BIT,VS_FULL_SPV,VS_FULL_SPV_N*4},{VK_SHADER_STAGE_FRAGMENT_BIT,FS_GREEN_SPV,FS_GREEN_SPV_N*4}};
   VkPipelineLayout pl; VkPipeline pipe=make_pipeline_ex(e,st,2,NULL,0,NULL,&pl);
   VkIndirectCommandsLayoutTokenEXT tok={.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,.type=VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT,.offset=0};
   VkIndirectCommandsLayoutCreateInfoEXT lci={.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,.shaderStages=VK_SHADER_STAGE_ALL_GRAPHICS,.indirectStride=sizeof(struct seq_draw),.pipelineLayout=pl,.tokenCount=1,.pTokens=&tok};
   VkIndirectCommandsLayoutEXT lay; VKCHECK(pCreateLayout(e->dev,&lci,NULL,&lay));

   struct seq_draw *seqs=calloc(MAX_SEQ,sizeof(*seqs));
   for(int i=0;i<MAX_SEQ;i++) seqs[i]=(struct seq_draw){3,1,0,0};
   VkBuffer sb; VkDeviceMemory sm; VkDeviceAddress sva=bda_usage_buffer(e,&sb,&sm,(VkDeviceSize)MAX_SEQ*sizeof(*seqs),VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); buf_write(e,sm,seqs,(VkDeviceSize)MAX_SEQ*sizeof(*seqs)); free(seqs);

   uint32_t cnt=count; VkBuffer cb; VkDeviceMemory cm; VkDeviceAddress cva=bda_usage_buffer(e,&cb,&cm,sizeof(cnt),VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); buf_write(e,cm,&cnt,sizeof(cnt));
   VkBuffer ppb; VkDeviceMemory ppm; VkDeviceSize ppsz=0; VkDeviceAddress ppva=preprocess_buffer(e,pMemReq,lay,pipe,MAX_SEQ,&ppsz,&ppb,&ppm);
   VkGeneratedCommandsPipelineInfoEXT pi={.sType=VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,.pipeline=pipe};
   VkGeneratedCommandsInfoEXT gi={.sType=VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,.pNext=&pi,.shaderStages=VK_SHADER_STAGE_ALL_GRAPHICS,.indirectCommandsLayout=lay,.indirectAddress=sva,.indirectAddressSize=(VkDeviceSize)MAX_SEQ*sizeof(*seqs),.preprocessAddress=ppva,.preprocessSize=ppsz,.sequenceCountAddress=cva,.maxSequenceCount=MAX_SEQ,.maxDrawCount=MAX_SEQ};
   VkCommandBufferBeginInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   VKCHECK(vkBeginCommandBuffer(e->cmd,&cbi)); vkCmdBindPipeline(e->cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe); begin_renderpass(e,C_MAGENTA);
   VkDeviceSize zero=0; vkCmdBindVertexBuffers(e->cmd,0,1,&e->vbo,&zero); pExecute(e->cmd,VK_FALSE,&gi); end_renderpass(e); VKCHECK(vkEndCommandBuffer(e->cmd));
   VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&e->cmd}; VKCHECK(vkQueueSubmit(e->queue,1,&si,e->fence)); VKCHECK(vkWaitForFences(e->dev,1,&e->fence,VK_TRUE,UINT64_MAX)); VKCHECK(vkResetFences(e->dev,1,&e->fence)); VKCHECK(vkResetCommandPool(e->dev,e->pool,0));
   void *map=readback(e);
   char what[64]; snprintf(what,sizeof(what),"count=%u/max=%u",count,MAX_SEQ);
   check(e,what,W/2,H/2,px(map,W/2,H/2),want);
}

int main(void)
{
   struct env e; struct env_ext ext={.dgc=VK_TRUE}; env_init_ex(&e,"probe25",&ext);
   pCreateLayout=(PFN_vkCreateIndirectCommandsLayoutEXT)vkGetDeviceProcAddr(e.dev,"vkCreateIndirectCommandsLayoutEXT");
   pMemReq=(PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)vkGetDeviceProcAddr(e.dev,"vkGetGeneratedCommandsMemoryRequirementsEXT");
   pExecute=(PFN_vkCmdExecuteGeneratedCommandsEXT)vkGetDeviceProcAddr(e.dev,"vkCmdExecuteGeneratedCommandsEXT");
   if(!pCreateLayout||!pMemReq||!pExecute) return 3;
   float verts[3][2]={{-0.008f,0.008f},{0.008f,0.008f},{0.0f,-0.016f}};
   buf_create(&e,&e.vbo,&e.vmem,sizeof(verts),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,1); buf_write(&e,e.vmem,verts,sizeof(verts));
   run_count(&e,0,C_MAGENTA);
   run_count(&e,2,C_GREEN);
   printf("probe25: %s (%d checks, %d fail)\n", e.fails?"FAIL":"ok", e.checks, e.fails);
   return e.fails?1:0;
}
