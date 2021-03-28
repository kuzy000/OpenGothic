#include "objectsbucket.h"

#include <Tempest/Log>

#include "graphics/mesh/pose.h"
#include "graphics/mesh/skeleton.h"
#include "sceneglobals.h"

#include "utils/workers.h"
#include "visualobjects.h"
#include "rendererstorage.h"

using namespace Tempest;

void ObjectsBucket::Item::setObjMatrix(const Tempest::Matrix4x4 &mt) {
  owner->setObjMatrix(id,mt);
  }

void ObjectsBucket::Item::setPose(const Pose &p) {
  owner->setPose(id,p);
  }

void ObjectsBucket::Item::setAsGhost(bool g) {
  if(owner->mat.isGhost==g)
    return;

  auto m = owner->mat;
  m.isGhost = g;
  auto&  bucket = owner->owner.getBucket(m,{},owner->boneCnt,owner->shaderType);

  auto&  v      = owner->val[id];
  size_t idNext = size_t(-1);
  switch(v.vboType) {
    case NoVbo:
      break;
    case VboVertex:{
      idNext = bucket.alloc(*v.vbo,*v.ibo,v.iboOffset,v.iboLength,v.visibility.bounds());
      break;
      }
    case VboVertexA:{
      idNext = bucket.alloc(*v.vboA,*v.ibo,v.iboOffset,v.iboLength,v.visibility.bounds());
      break;
      }
    case VboMorph:{
      idNext = bucket.alloc(v.vboM,v.visibility.bounds());
      break;
      }
    case VboMorpthGpu:{
      idNext = bucket.alloc(*v.vbo,*v.ibo,v.iboOffset,v.iboLength,v.visibility.bounds());
      break;
      }
    }
  if(idNext==size_t(-1))
    return;

  auto oldId = id;
  auto oldOw = owner;
  owner = &bucket;
  id    = idNext;

  auto& v2 = owner->val[id];
  setObjMatrix(v.pos);
  std::swap(v.timeShift, v2.timeShift);

  oldOw->free(oldId);
  }

const Bounds& ObjectsBucket::Item::bounds() const {
  if(owner!=nullptr)
    return owner->bounds(id);
  static Bounds b;
  return b;
  }

void ObjectsBucket::Item::draw(Tempest::Encoder<Tempest::CommandBuffer>& p, uint8_t fId) const {
  owner->draw(id,p,fId);
  }


void ObjectsBucket::Descriptors::invalidate() {
  for(size_t i=0;i<Resources::MaxFramesInFlight;++i)
    uboIsReady[i] = false;
  }

void ObjectsBucket::Descriptors::alloc(ObjectsBucket& owner) {
  auto& device = owner.scene.storage.device;
  for(size_t i=0;i<Resources::MaxFramesInFlight;++i) {
    if(owner.pMain!=nullptr)
      ubo[i][VisibilityGroup::V_Main] = device.uniforms(owner.pMain->layout());
    if(owner.pShadow!=nullptr) {
      for(size_t lay=VisibilityGroup::V_Shadow0; lay<=VisibilityGroup::V_ShadowLast; ++lay)
        ubo[i][lay] = device.uniforms(owner.pShadow->layout());
      }
    }
  }

ObjectsBucket::ObjectsBucket(const Material& mat, const std::vector<ProtoMesh::Animation>& anim, size_t boneCount,
                             VisualObjects& owner, const SceneGlobals& scene, Storage& storage, const Type type)
  :owner(owner), boneCnt(boneCount), scene(scene), storage(storage), mat(mat), shaderType(type) {
  static_assert(sizeof(UboPush)<=128, "UboPush is way too big");

  auto st = shaderType;
  if(anim.size()>0) {
    morphAnim = &anim;
    st        = Morph;
    }

  pMain    = scene.storage.materialPipeline(mat,st,RendererStorage::T_Forward );
  pGbuffer = scene.storage.materialPipeline(mat,st,RendererStorage::T_Deffered);
  pShadow  = scene.storage.materialPipeline(mat,st,RendererStorage::T_Shadow  );

  if(mat.frames.size()>0 || type==Animated || anim.size()>0)
    useSharedUbo = false; else
    useSharedUbo = true;

  textureInShadowPass = (mat.alpha==Material::AlphaTest);

  for(auto& i:uboMat) {
    UboMaterial zero;
    i = scene.storage.device.ubo<UboMaterial>(&zero,1);
    }

  if(useSharedUbo) {
    uboShared.invalidate();
    uboShared.alloc(*this);
    uboSetCommon(uboShared);
    }
  }

ObjectsBucket::~ObjectsBucket() {
  }

const Material& ObjectsBucket::material() const {
  return mat;
  }

ObjectsBucket::Object& ObjectsBucket::implAlloc(const VboType type, const Bounds& bounds) {
  Object* v = nullptr;
  for(size_t i=0; i<CAPACITY; ++i) {
    auto& vx = val[i];
    if(vx.isValid())
      continue;
    v = &vx;
    if(valLast<=i)
      valLast = i+1;
    break;
    }

  if(valSz==0)
    owner.resetIndex();

  ++valSz;
  v->vboType    = type;
  v->vbo        = nullptr;
  v->vboA       = nullptr;
  v->ibo        = nullptr;
  v->timeShift  = uint64_t(0-scene.tickCount);
  v->visibility = owner.visGroup.get();
  v->visibility.setBounds(bounds);

  if(!useSharedUbo) {
    v->ubo.invalidate();
    if(v->ubo.ubo[0][VisibilityGroup::V_Main].isEmpty()) {
      v->ubo.alloc(*this);
      uboSetCommon(v->ubo);
      }
    }
  return *v;
  }

void ObjectsBucket::uboSetCommon(Descriptors& v) {
  for(size_t i=0;i<Resources::MaxFramesInFlight;++i) {
    auto& t   = *mat.tex;
    auto& ubo = v.ubo[i][VisibilityGroup::V_Main];

    if(!ubo.isEmpty()) {
      ubo.set(0,t);
      ubo.set(1,*scene.shadowMap,Resources::shadowSampler());
      ubo.set(2,scene.uboGlobalPf[i][0]);
      ubo.set(4,uboMat[i]);
      if(isSceneInfoRequired()) {
        ubo.set(5,*scene.lightingBuf,Sampler2d::nearest());
        ubo.set(6,*scene.gbufDepth,  Sampler2d::nearest());
        }
      if(morphAnim!=nullptr) {
        ubo.set(7,(*morphAnim)[0].index);
        ubo.set(8,(*morphAnim)[0].samples);
        }
      }

    for(size_t lay=VisibilityGroup::V_Shadow0; lay<=VisibilityGroup::V_ShadowLast; ++lay) {
      auto& uboSh = v.ubo[i][lay];
      if(uboSh.isEmpty())
        continue;

      if(textureInShadowPass)
        uboSh.set(0,t);
      uboSh.set(2,scene.uboGlobalPf[i][lay]);
      uboSh.set(4,uboMat[i]);
      if(morphAnim!=nullptr) {
        uboSh.set(7,(*morphAnim)[0].index);
        uboSh.set(8,(*morphAnim)[0].samples);
        }
      }
    }
  }

void ObjectsBucket::uboSetDynamic(Object& v, uint8_t fId) {
  auto& ubo = v.ubo.ubo[fId][VisibilityGroup::V_Main];

  if(mat.frames.size()!=0) {
    auto frame = size_t((v.timeShift+scene.tickCount)/mat.texAniFPSInv);
    auto t = mat.frames[frame%mat.frames.size()];
    ubo.set(0,*t);
    if(pShadow!=nullptr && textureInShadowPass) {
      for(size_t lay=VisibilityGroup::V_Shadow0; lay<=VisibilityGroup::V_ShadowLast; ++lay) {
        auto& uboSh = v.ubo.ubo[fId][lay];
        uboSh.set(0,*t);
        }
      }
    }

  if(morphAnim!=nullptr) {
    ubo.set(7,(*morphAnim)[v.morphAnimId].index);
    ubo.set(8,(*morphAnim)[v.morphAnimId].samples);
    }

  if(v.ubo.uboIsReady[fId])
    return;
  v.ubo.uboIsReady[fId] = true;
  if(v.storageAni!=size_t(-1)) {
    storage.ani.bind(ubo,3,fId,v.storageAni,boneCnt);
    if(pShadow!=nullptr) {
      for(size_t lay=VisibilityGroup::V_Shadow0; lay<=VisibilityGroup::V_ShadowLast; ++lay) {
        auto& uboSh = v.ubo.ubo[fId][lay];
        storage.ani.bind(uboSh,3,fId,v.storageAni,boneCnt);
        }
      }
    }
  }

void ObjectsBucket::setupUbo() {
  if(useSharedUbo) {
    uboShared.invalidate();
    uboSetCommon(uboShared);
    } else {
    for(auto& i:val) {
      i.ubo.invalidate();
      if(!i.ubo.ubo[0][VisibilityGroup::V_Main].isEmpty())
        uboSetCommon(i.ubo);
      }
    }
  }

void ObjectsBucket::invalidateUbo() {
  if(useSharedUbo) {
    uboShared.invalidate();
    } else {
    for(auto& i:val)
      i.ubo.invalidate();
    }
  }

void ObjectsBucket::preFrameUpdate(uint8_t fId) {
  UboMaterial ubo;
  if(mat.texAniMapDirPeriod.x!=0)
    ubo.texAniMapDir.x = float(scene.tickCount%std::abs(mat.texAniMapDirPeriod.x))/float(mat.texAniMapDirPeriod.x);
  if(mat.texAniMapDirPeriod.y!=0)
    ubo.texAniMapDir.y = float(scene.tickCount%std::abs(mat.texAniMapDirPeriod.y))/float(mat.texAniMapDirPeriod.y);

  if(mat.texAniMapDirPeriod.x!=0 || mat.texAniMapDirPeriod.y!=0)
    uboMat[fId].update(&ubo,0,1);
  }

bool ObjectsBucket::groupVisibility(const Frustrum& f) {
  if(shaderType!=Static)
    return true;

  if(allBounds.r<=0) {
    Tempest::Vec3 bbox[2] = {};
    bool          fisrt=true;
    for(size_t i=0;i<CAPACITY;++i) {
      if(!val[i].isValid())
        continue;
      auto& b = val[i].visibility.bounds();
      if(fisrt) {
        bbox[0] = b.bboxTr[0];
        bbox[1] = b.bboxTr[1];
        fisrt = false;
        }
      bbox[0].x = std::min(bbox[0].x,b.bboxTr[0].x);
      bbox[0].y = std::min(bbox[0].y,b.bboxTr[0].y);
      bbox[0].z = std::min(bbox[0].z,b.bboxTr[0].z);

      bbox[1].x = std::max(bbox[1].x,b.bboxTr[1].x);
      bbox[1].y = std::max(bbox[1].y,b.bboxTr[1].y);
      bbox[1].z = std::max(bbox[1].z,b.bboxTr[1].z);
      }
    allBounds.assign(bbox);
    }
  return f.testPoint(allBounds.midTr,allBounds.r);
  }

size_t ObjectsBucket::alloc(const Tempest::VertexBuffer<Vertex>&  vbo,
                            const Tempest::IndexBuffer<uint32_t>& ibo,
                            size_t iboOffset, size_t iboLen,
                            const Bounds& bounds) {
  Object* v = &implAlloc(VboType::VboVertex,bounds);
  v->vbo       = &vbo;
  v->ibo       = &ibo;
  v->iboOffset = iboOffset;
  v->iboLength = iboLen;

  polySz+=ibo.size();
  polyAvg = polySz/valSz;
  return std::distance(val,v);
  }

size_t ObjectsBucket::alloc(const Tempest::VertexBuffer<VertexA>& vbo,
                            const Tempest::IndexBuffer<uint32_t>& ibo,
                            size_t iboOffset, size_t iboLen,
                            const Bounds& bounds) {
  Object* v = &implAlloc(VboType::VboVertexA,bounds);
  v->vboA      = &vbo;
  v->ibo       = &ibo;
  v->iboOffset = iboOffset;
  v->iboLength = iboLen;

  v->storageAni = storage.ani.alloc(boneCnt);
  polySz+=ibo.size();
  polyAvg = polySz/valSz;
  return std::distance(val,v);
  }

size_t ObjectsBucket::alloc(const Tempest::VertexBuffer<ObjectsBucket::Vertex>* vbo[], const Bounds& bounds) {
  Object* v = &implAlloc(VboType::VboMorph,bounds);
  for(size_t i=0; i<Resources::MaxFramesInFlight; ++i)
    v->vboM[i] = vbo[i];
  return std::distance(val,v);
  }

void ObjectsBucket::free(const size_t objId) {
  auto& v = val[objId];
  v.visibility = VisibilityGroup::Token();
  if(v.storageAni!=size_t(-1))
    storage.ani.free(v.storageAni,boneCnt);
  if(v.ibo!=nullptr)
    polySz -= v.ibo->size();
  v.vboType = VboType::NoVbo;
  v.vbo     = nullptr;
  for(size_t i=0;i<Resources::MaxFramesInFlight;++i)
    v.vboM[i] = nullptr;
  v.vboA    = nullptr;
  v.ibo     = nullptr;
  valSz--;
  valLast = 0;
  for(size_t i=CAPACITY; i>0;) {
    --i;
    if(val[i].isValid()) {
      valLast = i+1;
      break;
      }
    }
  if(valSz>0)
    polyAvg = polySz/valSz; else
    polyAvg = 0;

  if(valSz==0)
    owner.resetIndex();
  }

void ObjectsBucket::draw(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId) {
  if(pMain==nullptr)
    return;
  drawCommon(cmd,fId,*pMain,VisibilityGroup::V_Main);
  }

void ObjectsBucket::drawGBuffer(Tempest::Encoder<CommandBuffer>& cmd, uint8_t fId) {
  if(pGbuffer==nullptr)
    return;
  drawCommon(cmd,fId,*pGbuffer,VisibilityGroup::V_Main);
  }

void ObjectsBucket::drawShadow(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId, int layer) {
  if(pShadow==nullptr)
    return;
  drawCommon(cmd,fId,*pShadow,VisibilityGroup::VisCamera(VisibilityGroup::V_Shadow0+layer));
  }

void ObjectsBucket::drawCommon(Tempest::Encoder<CommandBuffer>& cmd, uint8_t fId,
                               const Tempest::RenderPipeline& shader, VisibilityGroup::VisCamera c) {
  UboPush pushBlock = {};
  bool    sharedSet = false;

  for(size_t i=0; i<valLast; ++i) {
    auto& v = val[i];
    if(v.vboType==NoVbo)
      continue;
    if(v.vboType!=VboMorph && !v.visibility.isVisible(c))
      continue;

    updatePushBlock(pushBlock,v);
    if(!useSharedUbo) {
      uboSetDynamic(v,fId);
      cmd.setUniforms(shader, v.ubo.ubo[fId][c], &pushBlock, sizeof(pushBlock));
      }
    else if(!sharedSet) {
      sharedSet = true;
      cmd.setUniforms(shader, uboShared.ubo[fId][c], &pushBlock, sizeof(pushBlock));
      }
    else {
      cmd.setUniforms(shader, &pushBlock, sizeof(pushBlock));
      }

    switch(v.vboType) {
      case VboType::NoVbo:
        break;
      case VboType::VboVertex:
        cmd.draw(*v.vbo, *v.ibo, v.iboOffset, v.iboLength);
        break;
      case VboType::VboVertexA:
        cmd.draw(*v.vboA,*v.ibo, v.iboOffset, v.iboLength);
        break;
      case VboType::VboMorph:
        cmd.draw(*v.vboM[fId]);
        break;
      case VboType::VboMorpthGpu:
        cmd.draw(*v.vbo, *v.ibo, v.iboOffset, v.iboLength);
        break;
      }
    }
  }

void ObjectsBucket::draw(size_t id, Tempest::Encoder<Tempest::CommandBuffer>& p, uint8_t fId) {
  auto& v = val[id];
  if(v.vbo==nullptr || pMain==nullptr)
    return;

  storage.commitUbo(scene.storage.device,fId);

  UboPush pushBlock = {};
  updatePushBlock(pushBlock,v);

  auto& ubo = (useSharedUbo ? uboShared.ubo[fId] : v.ubo.ubo[fId])[VisibilityGroup::V_Main];
  if(!useSharedUbo) {
    ubo.set(0,*mat.tex);
    ubo.set(1,Resources::fallbackTexture(),Sampler2d::nearest());
    ubo.set(2,scene.uboGlobalPf[fId][0]);
    ubo.set(4,uboMat[fId]);
    }

  p.setUniforms(*pMain,ubo,&pushBlock,sizeof(pushBlock));
  switch(v.vboType) {
    case VboType::NoVbo:
      break;
    case VboType::VboVertex:
      p.draw(*v.vbo, *v.ibo, v.iboOffset, v.iboLength);
      break;
    case VboType::VboVertexA:
      p.draw(*v.vboA,*v.ibo, v.iboOffset, v.iboLength);
      break;
    case VboType::VboMorph:
      p.draw(*v.vboM[fId]);
      break;
    case VboType::VboMorpthGpu:
      p.draw(*v.vbo, *v.ibo, v.iboOffset, v.iboLength);
      break;
    }
  }

void ObjectsBucket::setObjMatrix(size_t i, const Matrix4x4& m) {
  auto& v = val[i];
  v.visibility.setObjMatrix(m);
  v.pos = m;

  if(shaderType==Static)
    allBounds.r = 0;
  }

void ObjectsBucket::setPose(size_t i, const Pose& p) {
  if(shaderType!=Animated)
    return;
  auto& v    = val[i];
  auto& skel = storage.ani.element(v.storageAni);
  auto& tr   = p.transform();
  std::memcpy(&skel,tr.data(),std::min(tr.size(),boneCnt)*sizeof(tr[0]));
  storage.ani.markAsChanged(v.storageAni);
  }

void ObjectsBucket::setBounds(size_t i, const Bounds& b) {
  val[i].visibility.setBounds(b);
  }

bool ObjectsBucket::isSceneInfoRequired() const {
  return mat.isGhost || mat.alpha==Material::Water || mat.alpha==Material::Ghost;
  }

void ObjectsBucket::updatePushBlock(ObjectsBucket::UboPush& push, ObjectsBucket::Object& v) {
  push.pos = v.pos;
  if(morphAnim!=nullptr) {
    auto&    anim = (*morphAnim)[v.morphAnimId];
    uint64_t time = (scene.tickCount+v.timeShift);

    push.samplesPerFrame = int32_t(anim.samplesPerFrame);
    push.morphFrame[0]   = int32_t((time/anim.tickPerFrame+0)%anim.numFrames);
    push.morphFrame[1]   = int32_t((time/anim.tickPerFrame+1)%anim.numFrames);
    push.morphAlpha      = float(time%anim.tickPerFrame)/float(anim.tickPerFrame);
    }
  }

const Bounds& ObjectsBucket::bounds(size_t i) const {
  return val[i].visibility.bounds();
  }

bool ObjectsBucket::Storage::commitUbo(Device& device, uint8_t fId) {
  return ani.commitUbo(device,fId) | mat.commitUbo(device,fId);
  }
