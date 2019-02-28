#pragma once

#include <string>
#include <memory>

#include <Tempest/Signal>
#include <daedalus/DaedalusVM.h>

#include "world/world.h"

class Gothic final {
  public:
    Gothic(int argc,const char** argv);

    bool isInGame() const;
    bool doStartMenu() const { return !noMenu; }

    void         setWorld(std::unique_ptr<World> &&w);
    const World* world() const { return wrld.get(); }
    World*       world() { return wrld.get(); }
    WorldView*   worldView() const;
    Npc*         player();

    void     pushPause();
    void     popPause();
    bool     isPause() const;

    uint64_t tickCount() const;
    void     tick(uint64_t dt);

    gtime    time() const { return  wrldTime; }
    void     setTime(gtime t);

    void     updateAnimation();

    auto     updateDialog(const WorldScript::DlgChoise& dlg, Npc& player, Npc& npc) -> std::vector<WorldScript::DlgChoise>;
    void     dialogExec  (const WorldScript::DlgChoise& dlg, Npc& player, Npc& npc);

    void     aiProcessInfos (Npc& player, Npc& npc);
    void     aiOuput        (Npc& player, const char* msg, uint32_t time);
    void     aiCloseDialog  ();
    bool     aiIsDlgFinished();

    void     printScreen(const char* msg, int x, int y, int time, const Tempest::Font &font);
    void     print      (const char* msg);

    Tempest::Signal<void(const std::string&)>        onSetWorld;
    Tempest::Signal<void(Npc&,Npc&)>                 onDialogProcess;
    Tempest::Signal<void(Npc&,const char*,uint32_t)> onDialogOutput;
    Tempest::Signal<void()>                          onDialogClose;
    Tempest::Signal<void(bool&)>                     isDialogClose;

    Tempest::Signal<void(const char*,int,int,int,const Tempest::Font&)> onPrintScreen;
    Tempest::Signal<void(const char*)>                                  onPrint;

    const std::string&                    path() const { return gpath; }
    const std::string&                    defaultWorld() const;
    std::unique_ptr<Daedalus::DaedalusVM> createVm(const char* datFile);

    static void debug(const ZenLoad::zCMesh &mesh, std::ostream& out);
    static void debug(const ZenLoad::PackedMesh& mesh, std::ostream& out);
    static void debug(const ZenLoad::PackedSkeletalMesh& mesh, std::ostream& out);

  private:
    std::string wdef, gpath;
    bool        noMenu=false;
    uint16_t    pauseSum=0;

    uint64_t               ticks=0, wrldTimePart=0;
    gtime                  wrldTime;
    std::unique_ptr<World> wrld;

    static const uint64_t  multTime;
    static const uint64_t  divTime;
  };
