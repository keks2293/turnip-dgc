/*
 * probe22 - sequenceCountAddress correctness for DGC fast paths.
 */
#include "pcommon.h"
#include "vs_full_spv.h"
#include "fs_green_spv.h"

static PFN_vkCreateIndirectCommandsLayoutEXT pCreateLayout;
static PFN_vkGetGeneratedCommandsMemoryRequirementsEXT pMemReq;
static PFN_vkCmdExecuteGeneratedCommandsEXT pExecute;

struct seq_draw { uint32_t vertex_count, instance_count, first_vertex, first_instance; };
struct seq_idx {
   uint64_t index_addr; uint32_t index_size_B; uint32_t index_format;
   uint32_t index_count, instance_count, first_index, vertex_offset, first_instance;
};

static void run_draw(struct env *e)
{
   struct stagex st[2]={{VK_SHADER_STAGE_VERTEX_BIT,VS_FULL_SPV,VS_FULL_SPV_N*4},{VK_SHADER_STAGE_FRAGMENT_BIT,FS_GREEN_SPV,FS_GREEN_SPV_N*4}};
   VkPipelineLayout pl; VkPipeline pipe=make_pipeline_ex(e,st,2,NULL,0,NULL,&pl);
   VkIndirectCommandsLayoutTokenEXT tok={.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,.type=VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT,.offset=0};
   VkIndirectCommandsLayoutCreateInfoEXT lci={.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,.shaderStages=VK_SHADER_STAGE_ALL_GRAPHICS,.indirectStride=sizeof(struct seq_draw),.pipelineLayout=pl,.tokenCount=1,.pTokens=&tok};
   VkIndirectCommandsLayoutEXT lay; VKCHECK(pCreateLayout(e->dev,&lci,NULL,&lay));
   struct seq_draw seqs[8]; for(int i=0;i<8;i++) seqs[i]=(struct seq_draw){3,1,0,0};
   VkBuffer sb; VkDeviceMemory sm; VkDeviceAddress sva=bda_usage_buffer(e,&sb,&sm,sizeof(seqs),VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); buf_write(e,sm,seqs,sizeof(seqs));
   uint32_t count=2; VkBuffer cb; VkDeviceMemory cm; VkDeviceAddress cva=bda_usage_buffer(e,&cb,&cm,sizeof(count),VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); buf_write(e,cm,&count,sizeof(count));
   VkBuffer ppb; VkDeviceMemory ppm; VkDeviceSize ppsz=0; VkDeviceAddress ppva=preprocess_buffer(e,pMemReq,lay,pipe,8,&ppsz,&ppb,&ppm);
   VkGeneratedCommandsPipelineInfoEXT pi={.sType=VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,.pipeline=pipe};
   VkGeneratedCommandsInfoEXT gi={.sType=VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,.pNext=&pi,.shaderStages=VK_SHADER_STAGE_ALL_GRAPHICS,.indirectCommandsLayout=lay,.indirectAddress=sva,.indirectAddressSize=sizeof(seqs),.preprocessAddress=ppva,.preprocessSize=ppsz,.sequenceCountAddress=cva,.maxSequenceCount=8,.maxDrawCount=8};
   VkCommandBufferBeginInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   VKCHECK(vkBeginCommandBuffer(e->cmd,&cbi)); vkCmdBindPipeline(e->cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe); begin_renderpass(e,C_MAGENTA);
   VkDeviceSize zero=0; vkCmdBindVertexBuffers(e->cmd,0,1,&e->vbo,&zero); pExecute(e->cmd,VK_FALSE,&gi); end_renderpass(e); VKCHECK(vkEndCommandBuffer(e->cmd));
   VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&e->cmd}; VKCHECK(vkQueueSubmit(e->queue,1,&si,e->fence)); VKCHECK(vkWaitForFences(e->dev,1,&e->fence,VK_TRUE,UINT64_MAX)); VKCHECK(vkResetFences(e->dev,1,&e->fence)); VKCHECK(vkResetCommandPool(e->dev,e->pool,0));
   void *map=readback(e); check(e,"draw/count",W/2,H/2,px(map,W/2,H/2),C_GREEN);
}

static void run_indexed(struct env *e)
{
   struct stagex st[2]={{VK_SHADER_STAGE_VERTEX_BIT,VS_FULL_SPV,VS_FULL_SPV_N*4},{VK_SHADER_STAGE_FRAGMENT_BIT,FS_GREEN_SPV,FS_GREEN_SPV_N*4}};
   VkPipelineLayout pl; VkPipeline pipe=make_pipeline_ex(e,st,2,NULL,0,NULL,&pl);
   uint16_t ibd[16]={0,1,2}; VkBuffer ib; VkDeviceMemory ibm; bda_usage_buffer(e,&ib,&ibm,sizeof(ibd),VK_BUFFER_USAGE_INDEX_BUFFER_BIT); buf_write(e,ibm,ibd,sizeof(ibd));
   VkBufferDeviceAddressInfo bai={.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer=ib}; VkDeviceAddress ibva=vkGetBufferDeviceAddress(e->dev,&bai);
   VkIndirectCommandsIndexBufferTokenEXT ibt={.mode=VK_INDIRECT_COMMANDS_INPUT_MODE_DXGI_INDEX_BUFFER_EXT};
   VkIndirectCommandsLayoutTokenEXT toks[2]={{.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,.type=VK_INDIRECT_COMMANDS_TOKEN_TYPE_INDEX_BUFFER_EXT,.offset=0,.data.pIndexBuffer=&ibt},{.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,.type=VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT,.offset=16}};
   VkIndirectCommandsLayoutCreateInfoEXT lci={.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,.shaderStages=VK_SHADER_STAGE_ALL_GRAPHICS,.indirectStride=sizeof(struct seq_idx),.pipelineLayout=pl,.tokenCount=2,.pTokens=toks};
   VkIndirectCommandsLayoutEXT lay; VKCHECK(pCreateLayout(e->dev,&lci,NULL,&lay));
   struct seq_idx seqs[8]; for(int i=0;i<8;i++) seqs[i]=(struct seq_idx){.index_addr=ibva,.index_size_B=sizeof(ibd),.index_format=57,.index_count=3,.instance_count=1};
   VkBuffer sb; VkDeviceMemory sm; VkDeviceAddress sva=bda_usage_buffer(e,&sb,&sm,sizeof(seqs),VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); buf_write(e,sm,seqs,sizeof(seqs));
   uint32_t count=3; VkBuffer cb; VkDeviceMemory cm; VkDeviceAddress cva=bda_usage_buffer(e,&cb,&cm,sizeof(count),VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); buf_write(e,cm,&count,sizeof(count));
   VkBuffer ppb; VkDeviceMemory ppm; VkDeviceSize ppsz=0; VkDeviceAddress ppva=preprocess_buffer(e,pMemReq,lay,pipe,8,&ppsz,&ppb,&ppm);
   VkGeneratedCommandsPipelineInfoEXT pi={.sType=VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,.pipeline=pipe};
   VkGeneratedCommandsInfoEXT gi={.sType=VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,.pNext=&pi,.shaderStages=VK_SHADER_STAGE_ALL_GRAPHICS,.indirectCommandsLayout=lay,.indirectAddress=sva,.indirectAddressSize=sizeof(seqs),.preprocessAddress=ppva,.preprocessSize=ppsz,.sequenceCountAddress=cva,.maxSequenceCount=8,.maxDrawCount=8};
   VkCommandBufferBeginInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   VKCHECK(vkBeginCommandBuffer(e->cmd,&cbi)); vkCmdBindPipeline(e->cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe); begin_renderpass(e,C_MAGENTA);
   VkDeviceSize zero=0; vkCmdBindVertexBuffers(e->cmd,0,1,&e->vbo,&zero); pExecute(e->cmd,VK_FALSE,&gi); end_renderpass(e); VKCHECK(vkEndCommandBuffer(e->cmd));
   VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&e->cmd}; VKCHECK(vkQueueSubmit(e->queue,1,&si,e->fence)); VKCHECK(vkWaitForFences(e->dev,1,&e->fence,VK_TRUE,UINT64_MAX)); VKCHECK(vkResetFences(e->dev,1,&e->fence)); VKCHECK(vkResetCommandPool(e->dev,e->pool,0));
   void *map=readback(e); check(e,"indexed/count",W/2,H/2,px(map,W/2,H/2),C_GREEN);
}

int main(void)
{
   struct env e; struct env_ext ext={.dgc=VK_TRUE}; env_init_ex(&e,"probe22",&ext);
   pCreateLayout=(PFN_vkCreateIndirectCommandsLayoutEXT)vkGetDeviceProcAddr(e.dev,"vkCreateIndirectCommandsLayoutEXT");
   pMemReq=(PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)vkGetDeviceProcAddr(e.dev,"vkGetGeneratedCommandsMemoryRequirementsEXT");
   pExecute=(PFN_vkCmdExecuteGeneratedCommandsEXT)vkGetDeviceProcAddr(e.dev,"vkCmdExecuteGeneratedCommandsEXT");
   if(!pCreateLayout||!pMemReq||!pExecute) return 3;
   float verts[3][2]={{-0.008f,0.008f},{0.008f,0.008f},{0.0f,-0.016f}};
   buf_create(&e,&e.vbo,&e.vmem,sizeof(verts),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,1); buf_write(&e,e.vmem,verts,sizeof(verts));
   run_draw(&e);
   run_indexed(&e);
   printf("probe22: %s (%d checks, %d fail)\n", e.fails?"FAIL":"ok", e.checks, e.fails);
   return e.fails?1:0;
}
