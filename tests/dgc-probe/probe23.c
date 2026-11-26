/*
 * probe23 - preprocess once, execute many times.
 */
#include "pcommon.h"
#include "vs_full_spv.h"
#include "fs_green_spv.h"
#include "vs_pc_spv.h"
#define PC_BYTES 16
static PFN_vkCreateIndirectCommandsLayoutEXT pCreateLayout;
static PFN_vkGetGeneratedCommandsMemoryRequirementsEXT pMemReq;
static PFN_vkCmdExecuteGeneratedCommandsEXT pExecute;
static PFN_vkCmdPreprocessGeneratedCommandsEXT pPreprocess;
struct seq_draw { uint32_t vertex_count, instance_count, first_vertex, first_instance; };
struct seq_pc { uint32_t pc[PC_BYTES/4]; struct seq_draw draw; };
enum kind { V_DRAW, V_PC_DRAW };
struct meas { uint64_t inner[32], total[32]; int n; };
static uint64_t submit_wait_ns(struct env *e){ uint64_t t0=now_ns(); VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&e->cmd}; VKCHECK(vkQueueSubmit(e->queue,1,&si,e->fence)); VKCHECK(vkWaitForFences(e->dev,1,&e->fence,VK_TRUE,UINT64_MAX)); VKCHECK(vkResetFences(e->dev,1,&e->fence)); VKCHECK(vkResetCommandBuffer(e->cmd,0)); return now_ns()-t0; }
static void run_variant(struct env *e, enum kind kind)
{
   VkPushConstantRange pcr={.stageFlags=VK_SHADER_STAGE_VERTEX_BIT,.offset=0,.size=PC_BYTES};
   struct stagex st_draw[2]={{VK_SHADER_STAGE_VERTEX_BIT,VS_FULL_SPV,VS_FULL_SPV_N*4},{VK_SHADER_STAGE_FRAGMENT_BIT,FS_GREEN_SPV,FS_GREEN_SPV_N*4}};
   struct stagex st_pc[2]={{VK_SHADER_STAGE_VERTEX_BIT,VS_PC_SPV,VS_PC_SPV_N*4},{VK_SHADER_STAGE_FRAGMENT_BIT,FS_GREEN_SPV,FS_GREEN_SPV_N*4}};
   VkPipelineLayout pl; VkPipeline pipe;
   if(kind==V_PC_DRAW) pipe=make_pipeline_ex(e,st_pc,2,NULL,0,&pcr,&pl); else pipe=make_pipeline_ex(e,st_draw,2,NULL,0,NULL,&pl);
   VkIndirectCommandsPushConstantTokenEXT pct={.updateRange={.stageFlags=VK_SHADER_STAGE_VERTEX_BIT,.offset=0,.size=PC_BYTES}};
   VkIndirectCommandsLayoutTokenEXT toks[2]; uint32_t nt=1; size_t stride=sizeof(struct seq_draw);
   if(kind==V_PC_DRAW){ stride=sizeof(struct seq_pc); nt=2;
     toks[0]=(VkIndirectCommandsLayoutTokenEXT){.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,.type=VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT,.offset=0,.data.pPushConstant=&pct};
     toks[1]=(VkIndirectCommandsLayoutTokenEXT){.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,.type=VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT,.offset=PC_BYTES};
   } else toks[0]=(VkIndirectCommandsLayoutTokenEXT){.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,.type=VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT,.offset=0};
   VkIndirectCommandsLayoutCreateInfoEXT lci={.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,.flags=VK_INDIRECT_COMMANDS_LAYOUT_USAGE_EXPLICIT_PREPROCESS_BIT_EXT,.shaderStages=VK_SHADER_STAGE_ALL_GRAPHICS,.indirectStride=(uint32_t)stride,.pipelineLayout=pl,.tokenCount=nt,.pTokens=toks};
   VkIndirectCommandsLayoutEXT lay; VKCHECK(pCreateLayout(e->dev,&lci,NULL,&lay));
   const uint32_t N=512,M=32; uint8_t *seqs=calloc(N,stride);
   for(uint32_t i=0;i<N;i++){ if(kind==V_PC_DRAW){ struct seq_pc s={{0x1000+i,1,2,3},{3,1,0,0}}; memcpy(seqs+i*stride,&s,sizeof(s)); } else { struct seq_draw d={3,1,0,0}; memcpy(seqs+i*stride,&d,sizeof(d)); } }
   VkBuffer sb; VkDeviceMemory sm; VkDeviceAddress sva=bda_usage_buffer(e,&sb,&sm,(VkDeviceSize)N*stride,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); buf_write(e,sm,seqs,(VkDeviceSize)N*stride); free(seqs);
   VkBuffer ppb; VkDeviceMemory ppm; VkDeviceSize ppsz=0; VkDeviceAddress ppva=preprocess_buffer(e,pMemReq,lay,pipe,N,&ppsz,&ppb,&ppm);
   VkGeneratedCommandsPipelineInfoEXT pi={.sType=VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,.pipeline=pipe};
   VkGeneratedCommandsInfoEXT gi={.sType=VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,.pNext=&pi,.shaderStages=VK_SHADER_STAGE_ALL_GRAPHICS,.indirectCommandsLayout=lay,.indirectAddress=sva,.indirectAddressSize=(VkDeviceSize)N*stride,.preprocessAddress=ppva,.preprocessSize=ppsz,.maxSequenceCount=N,.maxDrawCount=N};
   /* preprocess once */
   VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=e->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
   VkCommandBuffer pcmd; VKCHECK(vkAllocateCommandBuffers(e->dev,&cai,&pcmd));
   VkCommandBufferBeginInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   VKCHECK(vkBeginCommandBuffer(pcmd,&cbi)); pPreprocess(pcmd,&gi,pcmd); VKCHECK(vkEndCommandBuffer(pcmd));
   VkSubmitInfo psi={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&pcmd}; VKCHECK(vkQueueSubmit(e->queue,1,&psi,e->fence)); VKCHECK(vkWaitForFences(e->dev,1,&e->fence,VK_TRUE,UINT64_MAX)); VKCHECK(vkResetFences(e->dev,1,&e->fence)); vkFreeCommandBuffers(e->dev,e->pool,1,&pcmd);
   struct meas m={0};
   for(int r=0;r<7;r++){
      uint64_t t0=now_ns();
      VKCHECK(vkBeginCommandBuffer(e->cmd,&cbi)); vkCmdBindPipeline(e->cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe); begin_renderpass(e,C_MAGENTA);
      VkDeviceSize zero=0; vkCmdBindVertexBuffers(e->cmd,0,1,&e->vbo,&zero);
      uint64_t i0=now_ns();
      for(uint32_t k=0;k<M;k++) pExecute(e->cmd,VK_TRUE,&gi);
      uint64_t i1=now_ns();
      end_renderpass(e); VKCHECK(vkEndCommandBuffer(e->cmd)); uint64_t tw=submit_wait_ns(e);
      m.inner[m.n]=i1-i0; m.total[m.n]=(i1-t0)+tw; m.n++;
   }
   printf("%-8s N=%u M=%u | exec-many inner %.2f us total %.2f us | per-exec inner %.2f us total %.2f us\n",
          kind==V_PC_DRAW?"pc_draw":"draw",N,M,us(median_u64(m.inner,m.n)),us(median_u64(m.total,m.n)),us(median_u64(m.inner,m.n))/M,us(median_u64(m.total,m.n))/M);
}
int main(void){ struct env e; struct env_ext ext={.dgc=VK_TRUE}; env_init_ex(&e,"probe23",&ext);
 pCreateLayout=(PFN_vkCreateIndirectCommandsLayoutEXT)vkGetDeviceProcAddr(e.dev,"vkCreateIndirectCommandsLayoutEXT");
 pMemReq=(PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)vkGetDeviceProcAddr(e.dev,"vkGetGeneratedCommandsMemoryRequirementsEXT");
 pExecute=(PFN_vkCmdExecuteGeneratedCommandsEXT)vkGetDeviceProcAddr(e.dev,"vkCmdExecuteGeneratedCommandsEXT");
 pPreprocess=(PFN_vkCmdPreprocessGeneratedCommandsEXT)vkGetDeviceProcAddr(e.dev,"vkCmdPreprocessGeneratedCommandsEXT");
 if(!pCreateLayout||!pMemReq||!pExecute||!pPreprocess) return 3;
 float verts[3][2]={{-0.008f,0.008f},{0.008f,0.008f},{0.0f,-0.016f}}; buf_create(&e,&e.vbo,&e.vmem,sizeof(verts),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,1); buf_write(&e,e.vmem,verts,sizeof(verts));
 run_variant(&e,V_DRAW); run_variant(&e,V_PC_DRAW);
 printf("probe23: ok\n"); return 0; }
