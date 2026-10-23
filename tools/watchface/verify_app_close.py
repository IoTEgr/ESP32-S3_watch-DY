"""Exercise the production app-close hook with optional Recents adapters."""
from pathlib import Path
import subprocess
from render_lvgl import ROOT, CLANG, LLD, VC, SDK

BUILD=ROOT/'build/app-close-host'
SOURCE=ROOT/'components/brookesia_core/systems/phone/esp_brookesia_phone_manager.cpp'

HARNESS=r'''
#include <assert.h>
#include <stdio.h>
#define ESP_UTILS_CHECK_NULL_RETURN(p,r,...) do {if(!(p))return r;}while(0)
#define ESP_UTILS_CHECK_FALSE_RETURN(p,r,...) do {if(!(p))return r;}while(0)
#define ESP_UTILS_LOGD(...) ((void)0)
namespace base {struct App {};}
struct App:base::App {};
struct RecentsScreen {
 bool visible=false;
 mutable unsigned reads=0;
 bool checkVisible() const {++reads;return visible;}
};
struct Display {
 RecentsScreen *recents=nullptr;
 RecentsScreen *getRecentsScreen() {return recents;}
};
struct Manager {
 enum class Screen {MAIN,RECENTS_SCREEN};
 Display display;
 base::App *active=nullptr;
 unsigned changes=0;
 Screen screens[2];
 int fail_at=-1;
 base::App *getActiveApp() {return active;}
 bool processDisplayScreenChange(Screen s,void *) {
  assert(changes<2);screens[changes++]=s;return (int)changes!=fail_at;
 }
 bool processAppCloseExtra(base::App *app);
};
__PRODUCTION_FUNCTION__
int main() {
 App app,other;
 Manager watch;watch.active=&app;
 assert(watch.processAppCloseExtra(&app)); /* Recents disabled, as on this watch */
 assert(watch.changes==1 && watch.screens[0]==Manager::Screen::MAIN);
 for(unsigned i=0;i<100;++i) {
  watch.changes=0;assert(watch.processAppCloseExtra(&app));assert(watch.changes==1);
 }
 RecentsScreen recent;
 Manager hidden;hidden.active=&app;hidden.display.recents=&recent;
 assert(hidden.processAppCloseExtra(&app) && hidden.changes==1 && recent.reads==1);
 recent.visible=true;
 Manager visible;visible.active=&app;visible.display.recents=&recent;
 assert(visible.processAppCloseExtra(&app) && visible.changes==2);
 assert(visible.screens[0]==Manager::Screen::MAIN && visible.screens[1]==Manager::Screen::RECENTS_SCREEN);
 Manager inactive;inactive.active=&other;
 assert(inactive.processAppCloseExtra(&app) && !inactive.changes);
 Manager invalid;
 assert(!invalid.processAppCloseExtra(nullptr) && !invalid.changes);
 Manager main_failure;main_failure.active=&app;main_failure.fail_at=1;
 assert(!main_failure.processAppCloseExtra(&app) && main_failure.changes==1);
 Manager recent_failure;recent_failure.active=&app;recent_failure.display.recents=&recent;recent_failure.fail_at=2;
 assert(!recent_failure.processAppCloseExtra(&app) && recent_failure.changes==2);
 puts("APP_CLOSE=PASS (Recents disabled/hidden/visible, repeated exits, inactive/invalid app, screen-load errors)");
}
'''

def main():
    source=SOURCE.read_text(encoding='utf-8')
    start=source.index('bool Manager::processAppCloseExtra(')
    end=source.index('bool Manager::processDisplayScreenChange(',start)
    production=source[start:end]
    BUILD.mkdir(parents=True,exist_ok=True)
    (BUILD/'test.cpp').write_text(HARNESS.replace('__PRODUCTION_FUNCTION__',production),encoding='utf-8')
    flags=['--target=x86_64-pc-windows-msvc','-std=c++11','-O1','-D_CRT_SECURE_NO_WARNINGS']
    for p in [VC/'include',SDK/'Include/shared',SDK/'Include/um']:
        flags+=['-I',str(p)]
    subprocess.run([str(CLANG),*flags,'-c',str(BUILD/'test.cpp'),'-o',str(BUILD/'test.obj')],check=True)
    subprocess.run([str(LLD),'-flavor','link',str(BUILD/'test.obj'),'/out:'+str(BUILD/'test.exe'),
        '/subsystem:console','/defaultlib:libcmt','/defaultlib:oldnames','/defaultlib:kernel32',
        '/libpath:'+str(VC/'lib/amd64'),'/libpath:'+str(SDK/'Lib/winv6.3/um/x64')],check=True)
    result=subprocess.run([str(BUILD/'test.exe')],capture_output=True,text=True,check=True)
    print(result.stdout.strip())
    (ROOT/'output/watchface/app-close-verification.txt').write_text(result.stdout,encoding='utf-8')

if __name__=='__main__':main()
