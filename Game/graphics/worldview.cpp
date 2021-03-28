#include "worldview.h"

#include <Tempest/Application>

#include "graphics/mesh/submesh/packedmesh.h"
#include "game/globaleffects.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "utils/gthfont.h"
#include "rendererstorage.h"

using namespace Tempest;

WorldView::WorldView(const World &world, const PackedMesh &wmesh, const RendererStorage &storage)
  : owner(world),storage(storage),sGlobal(storage),visuals(storage.device,sGlobal),
    objGroup(visuals),pfxGroup(*this,sGlobal,visuals),land(*this,visuals,wmesh) {
  visuals.setWorld(owner);
  pfxGroup.resetTicks();
  }

WorldView::~WorldView() {
  // cmd buffers must not be in use
  storage.device.waitIdle();
  }

const LightSource &WorldView::mainLight() const {
  return sGlobal.sun;
  }

bool WorldView::isInPfxRange(const Vec3& pos) const {
  return pfxGroup.isInPfxRange(pos);
  }

void WorldView::tick(uint64_t /*dt*/) {
  auto pl = owner.player();
  if(pl!=nullptr) {
    pfxGroup.setViewerPos(pl->position());
    }
  }

void WorldView::setModelView(const Matrix4x4& viewProj, const Tempest::Matrix4x4* shadow, size_t shCount) {
  updateLight();
  sGlobal.setModelView(viewProj,shadow,shCount);
  }

void WorldView::setFrameGlobals(const Texture2d& shadow, uint64_t tickCount, uint8_t fId) {
  if(needToUpdateUbo || &shadow!=sGlobal.shadowMap) {
    needToUpdateUbo = false;
    // wait before update all descriptors
    sGlobal.storage.device.waitIdle();
    sGlobal.setShadowMap(shadow);
    visuals.setupUbo();
    }
  pfxGroup.tick(tickCount);
  sGlobal.lights.tick(tickCount);
  sGlobal .setTime(tickCount);
  sGlobal .commitUbo(fId);

  sGlobal.lights.preFrameUpdate(fId);
  visuals .preFrameUpdate(fId);
  pfxGroup.preFrameUpdate(fId);
  }

void WorldView::setGbuffer(const Texture2d& lightingBuf, const Texture2d& diffuse, const Texture2d& norm, const Texture2d& depth) {
  if(sGlobal.lightingBuf == &lightingBuf &&
     sGlobal.gbufDiffuse == &diffuse &&
     sGlobal.gbufNormals == &norm    &&
     sGlobal.gbufDepth   == &depth)
    return;
  sGlobal.lightingBuf = &lightingBuf;
  sGlobal.gbufDiffuse = &diffuse;
  sGlobal.gbufNormals = &norm;
  sGlobal.gbufDepth   = &depth;

  // wait before update all descriptors
  sGlobal.storage.device.waitIdle();
  setupUbo();
  }

void WorldView::dbgLights(DbgPainter& p) const {
  sGlobal.lights.dbgLights(p);
  }

void WorldView::visibilityPass(const Matrix4x4& main, const Matrix4x4* sh, size_t shCount) {
  visuals.visibilityPass(main,sh,shCount);
  }

void WorldView::drawShadow(Tempest::Encoder<CommandBuffer>& cmd, uint8_t fId, uint8_t layer) {
  visuals.drawShadow(cmd,fId,layer);
  }

void WorldView::drawGBuffer(Tempest::Encoder<CommandBuffer>& cmd, uint8_t fId) {
  visuals.drawGBuffer(cmd,fId);
  }

void WorldView::drawMain(Tempest::Encoder<CommandBuffer>& cmd, uint8_t fId) {
  visuals.draw(cmd,fId);
  }

void WorldView::drawLights(Tempest::Encoder<CommandBuffer>& cmd, uint8_t fId) {
  sGlobal.lights.draw(cmd,fId);
  }

MeshObjects::Mesh WorldView::addView(const char* visual, int32_t headTex, int32_t teethTex, int32_t bodyColor) {
  if(auto mesh=Resources::loadMesh(visual))
    return MeshObjects::Mesh(objGroup,*mesh,headTex,teethTex,bodyColor,false);
  return MeshObjects::Mesh();
  }

MeshObjects::Mesh WorldView::addItmView(const char* visual, int32_t material) {
  if(auto mesh=Resources::loadMesh(visual))
    return MeshObjects::Mesh(objGroup,*mesh,material,0,0,true);
  return MeshObjects::Mesh();
  }

MeshObjects::Mesh WorldView::addAtachView(const ProtoMesh::Attach& visual, const int32_t version) {
  return MeshObjects::Mesh(objGroup,visual,version,false);
  }

MeshObjects::Mesh WorldView::addStaticView(const char* visual) {
  if(auto mesh=Resources::loadMesh(visual))
    return MeshObjects::Mesh(objGroup,*mesh,0,0,0,true);
  return MeshObjects::Mesh();
  }

MeshObjects::Mesh WorldView::addDecalView(const ZenLoad::zCVobData& vob) {
  if(auto mesh=Resources::decalMesh(vob))
    return MeshObjects::Mesh(objGroup,*mesh,0,0,0,true);
  return MeshObjects::Mesh();
  }

void WorldView::updateLight() {
  // https://www.suncalc.org/#/52.4561,13.4033,5/2020.06.28/13:09/1/3
  const int64_t rise         = gtime( 4,45).toInt();
  const int64_t meridian     = gtime(13, 9).toInt();
  const int64_t set          = gtime(21,33).toInt();
  const int64_t midnight     = gtime(1,0,0).toInt();
  const int64_t now          = owner.time().timeInDay().toInt();
  const float   shadowLength = 0.56f;

  float pulse = 0.f;
  if(rise<=now && now<meridian){
    pulse =  0.f + float(now-rise)/float(meridian-rise);
    }
  else if(meridian<=now && now<set){
    pulse =  1.f - float(now-meridian)/float(set-meridian);
    }
  else if(set<=now){
    pulse =  0.f - float(now-set)/float(midnight-set);
    }
  else if(now<rise){
    pulse = -1.f + (float(now)/float(rise));
    }

  float k = float(now)/float(midnight);

  float a  = std::max(0.f,std::min(pulse*3.f,1.f));
  auto clr = Vec3(0.75f,0.75f,0.75f)*a;
  sGlobal.ambient  = Vec3(0.2f,0.2f,0.3f)*(1.f-a)+Vec3(0.25f,0.25f,0.25f)*a;

  float ax  = 360-360*std::fmod(k+0.25f,1.f);
  ax = ax*float(M_PI/180.0);

  sGlobal.sun.setDir(-std::sin(ax)*shadowLength, pulse, std::cos(ax)*shadowLength);
  sGlobal.sun.setColor(clr);
  visuals.setDayNight(std::min(std::max(pulse*3.f,0.f),1.f));
  }

void WorldView::setupUbo() {
  // cmd buffers must not be in use
  sGlobal.lights.setupUbo();
  visuals.setupUbo();
  }
