/*
 * probe21 - explicit preprocess perf/correctness.
 * Measures: preprocess once + execute preprocessed many times, vs
 * execute(isPreprocessed=false) on the same explicit-preprocess layout, vs
 * manual CPU loop. Focus: pc_draw generic path where preprocess should help.
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

enum kind { V_DRAW, V_PC_DRAW, V_INDEXED };
struct seq_draw { uint32_t vertex_count, instance_count, first_vertex, first_instance; };
struct seq_pc { uint32_t pc[PC_BYTES / 4]; struct seq_draw draw; };
struct seq_idx {
   uint64_t index_addr; uint32_t index_size_B; uint32_t index_format;
   uint32_t index_count, instance_count, first_index, vertex_offset, first_instance;
};
struct variant {
   const char *name; enum kind kind; size_t stride;
   VkPipeline pipe; VkPipelineLayout pl; VkIndirectCommandsLayoutEXT lay;
   VkDeviceAddress seq_va; VkBuffer ppb; VkDeviceMemory ppm;
   VkDeviceAddress pp_va; VkDeviceSize pp_size; VkBuffer ib;
};
struct meas { uint64_t inner[64], rec[64], total[64]; int n; };

static uint64_t submit_wait_ns(struct env *e)
{
   uint64_t t0 = now_ns();
   VkSubmitInfo si = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&e->cmd};
   VKCHECK(vkQueueSubmit(e->queue, 1, &si, e->fence));
   VKCHECK(vkWaitForFences(e->dev, 1, &e->fence, VK_TRUE, UINT64_MAX));
   VKCHECK(vkResetFences(e->dev, 1, &e->fence));
   VKCHECK(vkResetCommandBuffer(e->cmd, 0));
   return now_ns() - t0;
}

static VkIndirectCommandsLayoutEXT make_layout(struct env *e, enum kind kind, uint32_t stride, VkPipelineLayout pl)
{
   VkIndirectCommandsPushConstantTokenEXT pct = {.updateRange={.stageFlags=VK_SHADER_STAGE_VERTEX_BIT,.offset=0,.size=PC_BYTES}};
   VkIndirectCommandsIndexBufferTokenEXT ibt = {.mode=VK_INDIRECT_COMMANDS_INPUT_MODE_DXGI_INDEX_BUFFER_EXT};
   VkIndirectCommandsLayoutTokenEXT toks[2]; uint32_t n=1;
   switch (kind) {
   case V_DRAW:
      toks[0]=(VkIndirectCommandsLayoutTokenEXT){.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,.type=VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT,.offset=0};
      break;
   case V_PC_DRAW:
      toks[0]=(VkIndirectCommandsLayoutTokenEXT){.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,.type=VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT,.offset=0,.data.pPushConstant=&pct};
      toks[1]=(VkIndirectCommandsLayoutTokenEXT){.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,.type=VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT,.offset=PC_BYTES}; n=2; break;
   case V_INDEXED:
      toks[0]=(VkIndirectCommandsLayoutTokenEXT){.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,.type=VK_INDIRECT_COMMANDS_TOKEN_TYPE_INDEX_BUFFER_EXT,.offset=0,.data.pIndexBuffer=&ibt};
      toks[1]=(VkIndirectCommandsLayoutTokenEXT){.sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,.type=VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT,.offset=16}; n=2; break;
   }
   VkIndirectCommandsLayoutCreateInfoEXT lci = {
      .sType=VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,
      .flags=VK_INDIRECT_COMMANDS_LAYOUT_USAGE_EXPLICIT_PREPROCESS_BIT_EXT,
      .shaderStages=VK_SHADER_STAGE_ALL_GRAPHICS,.indirectStride=stride,.pipelineLayout=pl,.tokenCount=n,.pTokens=toks};
   VkIndirectCommandsLayoutEXT lay; VKCHECK(pCreateLayout(e->dev,&lci,NULL,&lay)); return lay;
}

static void verify(struct env *e, struct variant *v, uint32_t nseq, VkBool32 preprocessed, const char *tag)
{
   VkGeneratedCommandsPipelineInfoEXT pinfo={.sType=VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,.pipeline=v->pipe};
   VkGeneratedCommandsInfoEXT gi={.sType=VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,.pNext=&pinfo,.shaderStages=VK_SHADER_STAGE_ALL_GRAPHICS,
      .indirectCommandsLayout=v->lay,.indirectAddress=v->seq_va,.indirectAddressSize=(VkDeviceSize)v->stride*nseq,
      .preprocessAddress=v->pp_va,.preprocessSize=v->pp_size,.maxSequenceCount=nseq,.maxDrawCount=nseq};
   if (preprocessed) {
      VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=e->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
      VkCommandBuffer pcmd; VKCHECK(vkAllocateCommandBuffers(e->dev,&cai,&pcmd));
      VkCommandBufferBeginInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      VKCHECK(vkBeginCommandBuffer(pcmd,&cbi)); pPreprocess(pcmd,&gi,pcmd); VKCHECK(vkEndCommandBuffer(pcmd));
      VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&pcmd};
      VKCHECK(vkQueueSubmit(e->queue,1,&si,e->fence)); VKCHECK(vkWaitForFences(e->dev,1,&e->fence,VK_TRUE,UINT64_MAX)); VKCHECK(vkResetFences(e->dev,1,&e->fence));
      vkFreeCommandBuffers(e->dev,e->pool,1,&pcmd);
   }
   VkCommandBufferBeginInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   VKCHECK(vkBeginCommandBuffer(e->cmd,&cbi));
   vkCmdBindPipeline(e->cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,v->pipe);
   begin_renderpass(e,C_MAGENTA);
   VkDeviceSize zero=0; vkCmdBindVertexBuffers(e->cmd,0,1,&e->vbo,&zero);
   if (v->kind==V_INDEXED) vkCmdBindIndexBuffer(e->cmd,v->ib,0,VK_INDEX_TYPE_UINT16);
   pExecute(e->cmd,preprocessed,&gi);
   end_renderpass(e); VKCHECK(vkEndCommandBuffer(e->cmd)); submit_wait_ns(e);
   void *map=readback(e); check(e,tag,W/2,H/2,px(map,W/2,H/2),C_GREEN);
}

static struct meas measure(struct env *e, struct variant *v, uint32_t nseq, int mode, int reps)
{
   /* mode: 0=unpreprocessed execute, 1=preprocessed execute (preprocess once outside loop), 2=manual */
   struct meas m={0};
   VkGeneratedCommandsPipelineInfoEXT pinfo={.sType=VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,.pipeline=v->pipe};
   VkGeneratedCommandsInfoEXT gi={.sType=VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,.pNext=&pinfo,.shaderStages=VK_SHADER_STAGE_ALL_GRAPHICS,
      .indirectCommandsLayout=v->lay,.indirectAddress=v->seq_va,.indirectAddressSize=(VkDeviceSize)v->stride*nseq,
      .preprocessAddress=v->pp_va,.preprocessSize=v->pp_size,.maxSequenceCount=nseq,.maxDrawCount=nseq};
   uint32_t pc[PC_BYTES/4]={0x11223344,1,2,3};
   if (mode==1) {
      VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=e->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
      VkCommandBuffer pcmd; VKCHECK(vkAllocateCommandBuffers(e->dev,&cai,&pcmd));
      VkCommandBufferBeginInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      VKCHECK(vkBeginCommandBuffer(pcmd,&cbi)); pPreprocess(pcmd,&gi,pcmd); VKCHECK(vkEndCommandBuffer(pcmd));
      VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&pcmd};
      VKCHECK(vkQueueSubmit(e->queue,1,&si,e->fence)); VKCHECK(vkWaitForFences(e->dev,1,&e->fence,VK_TRUE,UINT64_MAX)); VKCHECK(vkResetFences(e->dev,1,&e->fence));
      vkFreeCommandBuffers(e->dev,e->pool,1,&pcmd);
   }
   for (int r=0;r<reps;r++) {
      VkCommandBufferBeginInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      VKCHECK(vkBeginCommandBuffer(e->cmd,&cbi));
      vkCmdBindPipeline(e->cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,v->pipe);
      uint64_t t0=now_ns();
      begin_renderpass(e,C_MAGENTA);
      VkDeviceSize zero=0; vkCmdBindVertexBuffers(e->cmd,0,1,&e->vbo,&zero);
      if (mode==2 && v->kind==V_INDEXED) vkCmdBindIndexBuffer(e->cmd,v->ib,0,VK_INDEX_TYPE_UINT16);
      uint64_t i0=now_ns(), i1;
      if (mode==2) {
         for (uint32_t i=0;i<nseq;i++) {
            if (v->kind==V_PC_DRAW) vkCmdPushConstants(e->cmd,v->pl,VK_SHADER_STAGE_VERTEX_BIT,0,PC_BYTES,pc);
            if (v->kind==V_INDEXED) vkCmdDrawIndexed(e->cmd,3,1,0,0,0); else vkCmdDraw(e->cmd,3,1,0,0);
         }
      } else {
         pExecute(e->cmd, mode==1?VK_TRUE:VK_FALSE, &gi);
      }
      i1=now_ns();
      vkCmdEndRenderPass(e->cmd); VKCHECK(vkEndCommandBuffer(e->cmd));
      uint64_t t1=now_ns(); uint64_t tw=submit_wait_ns(e);
      m.inner[m.n]=i1-i0; m.rec[m.n]=t1-t0; m.total[m.n]=t1-t0+tw; m.n++;
   }
   return m;
}

static void report(const char *name, uint32_t n, struct meas a, struct meas b, struct meas c)
{
   uint64_t ai=median_u64(a.inner,a.n), ar=median_u64(a.rec,a.n), at=median_u64(a.total,a.n);
   uint64_t bi=median_u64(b.inner,b.n), br=median_u64(b.rec,b.n), bt=median_u64(b.total,b.n);
   uint64_t ci=median_u64(c.inner,c.n), cr=median_u64(c.rec,c.n), ct=median_u64(c.total,c.n);
   printf("%-8s %4u | unpre %8.2f %8.2f %8.2f | pre %8.2f %8.2f %8.2f | man %8.2f %8.2f %8.2f | pre/unpre %.2f man/pre %.2f\n",
          name,n,us(ai),us(ar),us(at),us(bi),us(br),us(bt),us(ci),us(cr),us(ct),
          ai? (double)bi/ai:0.0, bi? (double)ci/bi:0.0);
}

int main(void)
{
   struct env e; struct env_ext ext={.dgc=VK_TRUE}; env_init_ex(&e,"probe21",&ext);
   pCreateLayout=(PFN_vkCreateIndirectCommandsLayoutEXT)vkGetDeviceProcAddr(e.dev,"vkCreateIndirectCommandsLayoutEXT");
   pMemReq=(PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)vkGetDeviceProcAddr(e.dev,"vkGetGeneratedCommandsMemoryRequirementsEXT");
   pExecute=(PFN_vkCmdExecuteGeneratedCommandsEXT)vkGetDeviceProcAddr(e.dev,"vkCmdExecuteGeneratedCommandsEXT");
   pPreprocess=(PFN_vkCmdPreprocessGeneratedCommandsEXT)vkGetDeviceProcAddr(e.dev,"vkCmdPreprocessGeneratedCommandsEXT");
   if(!pCreateLayout||!pMemReq||!pExecute||!pPreprocess) return 3;

   struct stagex st_plain[2]={{VK_SHADER_STAGE_VERTEX_BIT,VS_FULL_SPV,VS_FULL_SPV_N*4},{VK_SHADER_STAGE_FRAGMENT_BIT,FS_GREEN_SPV,FS_GREEN_SPV_N*4}};
   struct stagex st_pc[2]={{VK_SHADER_STAGE_VERTEX_BIT,VS_PC_SPV,VS_PC_SPV_N*4},{VK_SHADER_STAGE_FRAGMENT_BIT,FS_GREEN_SPV,FS_GREEN_SPV_N*4}};
   VkPushConstantRange pcr={.stageFlags=VK_SHADER_STAGE_VERTEX_BIT,.offset=0,.size=PC_BYTES};
   VkPipelineLayout pl_plain,pl_pc; VkPipeline pipe_plain=make_pipeline_ex(&e,st_plain,2,NULL,0,NULL,&pl_plain);
   VkPipeline pipe_pc=make_pipeline_ex(&e,st_pc,2,NULL,0,&pcr,&pl_pc);

   float verts[3][2]={{-0.008f,0.008f},{0.008f,0.008f},{0.0f,-0.016f}};
   buf_create(&e,&e.vbo,&e.vmem,sizeof(verts),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,1); buf_write(&e,e.vmem,verts,sizeof(verts));
   uint16_t ib[16]={0,1,2}; VkBuffer ibo; VkDeviceMemory ibom; bda_usage_buffer(&e,&ibo,&ibom,sizeof(ib),VK_BUFFER_USAGE_INDEX_BUFFER_BIT); buf_write(&e,ibom,ib,sizeof(ib));
   VkBufferDeviceAddressInfo ibai={.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer=ibo}; VkDeviceAddress ib_va=vkGetBufferDeviceAddress(e.dev,&ibai);

   const uint32_t NMAX=512;
   struct variant v[3]={{"draw",V_DRAW,sizeof(struct seq_draw),pipe_plain,pl_plain},{ "pc_draw",V_PC_DRAW,sizeof(struct seq_pc),pipe_pc,pl_pc},{ "indexed",V_INDEXED,sizeof(struct seq_idx),pipe_plain,pl_plain}};
   for(int i=0;i<3;i++){
      v[i].ib=ibo;
      VkBuffer sb; VkDeviceMemory sm; VkDeviceSize sz=(VkDeviceSize)v[i].stride*NMAX; uint8_t *seqs=calloc(NMAX,v[i].stride);
      for(uint32_t k=0;k<NMAX;k++){ uint8_t *p=seqs+k*v[i].stride;
         if(v[i].kind==V_DRAW){ struct seq_draw d={3,1,0,0}; memcpy(p,&d,sizeof(d)); }
         else if(v[i].kind==V_PC_DRAW){ struct seq_pc s={{0x1000+k,1,2,3},{3,1,0,0}}; memcpy(p,&s,sizeof(s)); }
         else { struct seq_idx s={.index_addr=ib_va,.index_size_B=sizeof(ib),.index_format=57,.index_count=3,.instance_count=1}; memcpy(p,&s,sizeof(s)); }
      }
      v[i].seq_va=bda_usage_buffer(&e,&sb,&sm,sz,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); buf_write(&e,sm,seqs,sz); free(seqs);
      v[i].lay=make_layout(&e,v[i].kind,(uint32_t)v[i].stride,v[i].pl);
      v[i].pp_va=preprocess_buffer(&e,pMemReq,v[i].lay,v[i].pipe,NMAX,&v[i].pp_size,&v[i].ppb,&v[i].ppm);
   }

   for(int i=0;i<3;i++){ char t[64]; snprintf(t,sizeof(t),"%s/unpre",v[i].name); verify(&e,&v[i],4,VK_FALSE,t); snprintf(t,sizeof(t),"%s/pre",v[i].name); verify(&e,&v[i],4,VK_TRUE,t); }

   printf("\n%-8s %4s | %-28s | %-28s | %-28s | ratios\n","variant","N","unpre inner rec total","pre inner rec total","man inner rec total");
   static const uint32_t NS[]={1,64,512}; int NNS=3;
   for(int i=0;i<3;i++){
      for(int j=0;j<NNS;j++){ uint32_t n=NS[j]; int reps=n<=8?15:(n<=64?9:5);
         struct meas mu=measure(&e,&v[i],n,0,reps); struct meas mp=measure(&e,&v[i],n,1,reps); struct meas mm=measure(&e,&v[i],n,2,reps);
         report(v[i].name,n,mu,mp,mm);
      }
   }
   printf("probe21: %d checks, %d fail\n",e.checks,e.fails);
   return e.fails?1:0;
}
