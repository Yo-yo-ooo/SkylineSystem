#pragma once
#include "../../WindowStuff/Window/window.h"
#include "../../WindowStuff/SubInstances/guiInstance/guiInstance.h"
#include "../../WindowStuff/SubInstances/guiInstance/guiStuff/components/box/boxComponent.h"
#include "../../WindowStuff/SubInstances/guiInstance/guiStuff/components/textField/textFieldComponent.h"
#include "../../WindowStuff/SubInstances/guiInstance/guiStuff/components/button/buttonComponent.h"

namespace SysApps
{
    class Explorer
    {
        private:
        Window* window;
        GuiInstance* guiInstance;
        GuiComponentStuff::BaseComponent* lastClickedComp;
        GuiComponentStuff::BoxComponent* fileListComp;
        GuiComponentStuff::TextFieldComponent* pathComp;
        GuiComponentStuff::ButtonComponent* goUpBtn;
        List<void*> folderCompsYes = List<void*>(4);
        List<const char*> folderPathsYes = List<const char*>(4);
        List<void*> driveCompsYes = List<void*>(4);
        List<const char*> drivePathsYes = List<const char*>(4);
        
        const char* path;
        int ScrollY;


        public:
        Explorer();
        void Explorer::UpdateSizes();
        const char* GetPath();
        void SetPath(const char* path);
        void Reload();

        void ClickCallBack(GuiComponentStuff::BaseComponent* comp, GuiComponentStuff::MouseClickEventInfo event);
        bool PathTypeCallBack(GuiComponentStuff::TextFieldComponent* comp, GuiComponentStuff::KeyHitEventInfo event);

        void OnExternalWindowClose(Window* window);
        void OnExternalWindowResize(Window* window);

        void OnFolderClick(GuiComponentStuff::ButtonComponent* btn, GuiComponentStuff::MouseClickEventInfo info);
        void OnFileClick(GuiComponentStuff::ButtonComponent* btn, GuiComponentStuff::MouseClickEventInfo info);
        void OnDriveClick(GuiComponentStuff::ButtonComponent* btn, GuiComponentStuff::MouseClickEventInfo info);
        void OnGoUpClick(GuiComponentStuff::ButtonComponent* btn, GuiComponentStuff::MouseClickEventInfo info);

        void Free();
        void ClearLists();
    };



}