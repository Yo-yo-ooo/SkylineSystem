#include "guiInstance.h"
#include "guiStuff/components/box/boxComponent.h"
#include "guiStuff/components/button/buttonComponent.h"
#include "guiStuff/components/screenComponent/screenComponent.h"
#include "guiStuff/components/textField/textFieldComponent.h"
#include "../../../memory/heap.h"
#include "../../../cmdParsing/cstrTools.h"

GuiInstance::GuiInstance(Window* window)
{
    this->window = window;
    instanceType = InstanceType::GUI;
}



void GuiInstance::Init()
{
    //window->renderer->Clear(Colors.white);
    allComponents = new List<GuiComponentStuff::BaseComponent*>(10);
    screen = new GuiComponentStuff::ScreenComponent(window);
    allComponents->add(screen);
    screen->id = 1234;
    window->renderer->Clear(Colors.white);
}

int GetBaseComponentAttributeSize(GuiInstanceBaseAttributeType type)
{
    if (type == GuiInstanceBaseAttributeType::POSITION_X)
        return 4;
    if (type == GuiInstanceBaseAttributeType::POSITION_Y)
        return 4;
    if (type == GuiInstanceBaseAttributeType::SIZE_FIXED_X)
        return 4;
    if (type == GuiInstanceBaseAttributeType::SIZE_FIXED_Y)
        return 4;
    if (type == GuiInstanceBaseAttributeType::SIZE_SCALED_X)
        return 8;
    if (type == GuiInstanceBaseAttributeType::SIZE_SCALED_Y)
        return 8;
    if (type == GuiInstanceBaseAttributeType::SIZE_IS_FIXED_X)
        return 1;
    if (type == GuiInstanceBaseAttributeType::SIZE_IS_FIXED_Y)
        return 1;
    if (type == GuiInstanceBaseAttributeType::ACTUAL_SIZE_X)
        return 4;
    if (type == GuiInstanceBaseAttributeType::ACTUAL_SIZE_Y)
        return 4;

    if (type == GuiInstanceBaseAttributeType::ID)
        return 8;
    if (type == GuiInstanceBaseAttributeType::PARENT_ID)
        return 8;
    if (type == GuiInstanceBaseAttributeType::IS_HIDDEN)
        return 1;

    return 0;
}

void GuiInstance::Render()
{
    if (screen == NULL)
        return;
    //window->renderer->Clear(Colors.orange);
    screen->Render(GuiComponentStuff::Field(GuiComponentStuff::Position(), GuiComponentStuff::Position(window->size.width - 1, window->size.height - 1)));
    long t = window->size.height * (long)window->size.width;

    for (long i = 0; i < t; i++)
        ((uint32_t*)window->framebuffer->BaseAddress)[i] = screen->renderer->componentFrameBuffer->pixels[i];
}

GuiComponentStuff::BaseComponent* GuiInstance::GetComponentFromId(uint64_t id)
{
    if (screen == NULL)
        return NULL;

    if (screen->id == id)
        return screen;

    for (int i = 0; i < allComponents->getCount(); i++)
    {
        GuiComponentStuff::BaseComponent* comp = allComponents->elementAt(i);
        if (comp == NULL)
            continue;
        if (comp->id == id)
            return comp;
    }

    return NULL;
}

GuiComponentStuff::BaseComponent* GuiInstance::GetChildFromComponentWithId(uint64_t id, int index)
{
    GuiComponentStuff::BaseComponent* base = GetComponentFromId(id);
    if (base == NULL)
        return NULL;
    if (index < 0)
        return NULL;

    if (base->componentType == GuiComponentStuff::ComponentType::BOX)
    {
        GuiComponentStuff::BoxComponent* box = (GuiComponentStuff::BoxComponent*)base;
        if (index >= box->children->getCount())
            return NULL;
        return box->children->elementAt(index);
    }

    if (base->componentType == GuiComponentStuff::ComponentType::SCREEN)
    {
        GuiComponentStuff::ScreenComponent* scr = (GuiComponentStuff::ScreenComponent*)base;
        if (index >= scr->children->getCount())
            return NULL;
        return scr->children->elementAt(index);
    }

    if (base->componentType == GuiComponentStuff::ComponentType::BUTTON)
    {
        GuiComponentStuff::ButtonComponent* btn = (GuiComponentStuff::ButtonComponent*)base;
        if (index >= 1)
            return NULL;
        return btn->actualButtonStuff;
    }

    if (base->componentType == GuiComponentStuff::ComponentType::TEXTFIELD)
    {
        GuiComponentStuff::TextFieldComponent* txt = (GuiComponentStuff::TextFieldComponent*)base;
        if (index >= 1)
            return NULL;
        return txt->actualTextFieldStuff;
    }

    return NULL;
}

int GuiInstance::GetIndexOfChildFromComponentWithId(uint64_t id, uint64_t childId)
{
    GuiComponentStuff::BaseComponent* base = GetComponentFromId(id);
    GuiComponentStuff::BaseComponent* child = GetComponentFromId(childId);
    if (base == NULL)
        return -1;
    if (child == NULL)
        return -1;

    

    if (base->componentType == GuiComponentStuff::ComponentType::BOX)
    {
        GuiComponentStuff::BoxComponent* box = (GuiComponentStuff::BoxComponent*)base;
        return box->children->getIndexOf(child);
    }

    if (base->componentType == GuiComponentStuff::ComponentType::SCREEN)
    {
        GuiComponentStuff::ScreenComponent* scr = (GuiComponentStuff::ScreenComponent*)base;
        return scr->children->getIndexOf(child);
    }

    return -1;
}

bool GuiInstance::RemoveChildFromComponentWithId(uint64_t id, int index)
{
    GuiComponentStuff::BaseComponent* base = GetComponentFromId(id);
    
    if (base == NULL)
        return false;
    if (index < 0)
        return false;
        

    if (base->componentType == GuiComponentStuff::ComponentType::BOX)
    {
        GuiComponentStuff::BoxComponent* box = (GuiComponentStuff::BoxComponent*)base;
        if (index >= box->children->getCount() || index < 0)
            return false;
        box->children->removeAt(index);
        return true;
    }

    if (base->componentType == GuiComponentStuff::ComponentType::SCREEN)
    {
        GuiComponentStuff::ScreenComponent* scr = (GuiComponentStuff::ScreenComponent*)base;
        if (index >= scr->children->getCount())
            return false;
        scr->children->removeAt(index);
        return true;
    }

    return false;
}

//, void (*callBackFunc)(BaseComponent* comp)

GuiInstance* currentInst;

void RemoveThingFromList(GuiComponentStuff::BaseComponent* comp)
{
    int indx = currentInst->allComponents->getIndexOf(comp);
    if (indx != -1)
        currentInst->allComponents->removeAt(indx);
}

#include "../../../OSDATA/MStack/MStackM.h"

void GuiInstance::Free() 
{
    AddToStack();
    GuiComponentStuff::ScreenComponent* tScreen = screen;
    screen = NULL;
    currentInst = this;
    RemoveThingFromList(tScreen);
    tScreen->Destroy(true, RemoveThingFromList);
    _Free(tScreen);
    
   for (int i = 0; i < allComponents->getCount(); i++)
    {
        if (allComponents->elementAt(i)->componentType != GuiComponentStuff::ComponentType::SCREEN)
            continue;
        GuiComponentStuff::ScreenComponent* bruh = (GuiComponentStuff::ScreenComponent*)allComponents->elementAt(i);
        RemoveThingFromList(bruh);
        bruh->Destroy(true, RemoveThingFromList);
        _Free(bruh);
        i = -1;
    }

    allComponents->free();
    _Free(allComponents);
    RemoveFromStack();
}

bool GuiInstance::DeleteComponentWithId(int64_t id, bool destroyChildren)
{
    GuiComponentStuff::BaseComponent* base = GetComponentFromId(id);
    if (base == NULL)
        return false;
    if (screen == base)
        return false;
    currentInst = this;
  
    bool res = base->Destroy(destroyChildren, RemoveThingFromList);
    RemoveThingFromList(base);
    _Free(base);
    return res;
}


bool GuiInstance::SetActiveScreenFromId(int64_t id)
{
    GuiComponentStuff::BaseComponent* base = GetComponentFromId(id);
    if (base == NULL)
        return false;
    if (base->componentType != GuiComponentStuff::ComponentType::SCREEN)
        return false;

    screen = (GuiComponentStuff::ScreenComponent*)base;
    return true;
}

bool GuiInstance::SetSpecificComponentAttribute(int64_t id, int32_t type, uint64_t val)
{
    GuiComponentStuff::BaseComponent* base = GetComponentFromId(id);
    if (base == NULL)
        return false;

    return base->SetAttribute(type, val);
}

uint64_t GuiInstance::GetSpecificComponentAttribute(int64_t id, int32_t type)
{
    GuiComponentStuff::BaseComponent* base = GetComponentFromId(id);
    if (base == NULL)
        return 0;

    return base->GetAttribute(type);
}
int GuiInstance::GetSpecificComponentAttributeSize(int64_t id, int32_t type)
{
    GuiComponentStuff::BaseComponent* base = GetComponentFromId(id);
    if (base == NULL)
        return 0;

    return base->GetAttributeSize(type);
}

bool GuiInstance::CreateComponentWithId(int64_t id, GuiComponentStuff::ComponentType type)
{
    if (screen == NULL)
        return false;
    return CreateComponentWithIdAndParent(id, type, screen->id);
}




bool GuiInstance::SetBaseComponentAttribute(int64_t id, GuiInstanceBaseAttributeType type, uint64_t val)
{
    GuiComponentStuff::BaseComponent* base = GetComponentFromId(id);
    if (base == NULL)
        return false;

    if (type == GuiInstanceBaseAttributeType::POSITION_X)
    {
        base->position.x = *((int32_t*)&val);
        return true;
    }

    if (type == GuiInstanceBaseAttributeType::POSITION_Y)
    {
        base->position.y = *((int32_t*)&val);
        return true;
    }

    if (type == GuiInstanceBaseAttributeType::SIZE_FIXED_X)
    {
        base->size.FixedX = *((int32_t*)&val);
        return true;
    }

    if (type == GuiInstanceBaseAttributeType::SIZE_FIXED_Y)
    {
        base->size.FixedY = *((int32_t*)&val);
        return true;
    }

    if (type == GuiInstanceBaseAttributeType::ACTUAL_SIZE_X)
    {
        //base->GetActualComponentSize().FixedX = *((int32_t*)&val);
        return false;
    }

    if (type == GuiInstanceBaseAttributeType::ACTUAL_SIZE_Y)
    {
        //base->GetActualComponentSize().FixedY = *((int32_t*)&val);
        return false;
    }

    if (type == GuiInstanceBaseAttributeType::SIZE_SCALED_X)
    {
        base->size.ScaledX = *((double*)&val);
        return true;
    }

    if (type == GuiInstanceBaseAttributeType::SIZE_SCALED_Y)
    {
        base->size.ScaledY = *((double*)&val);
        return true;
    }

    if (type == GuiInstanceBaseAttributeType::SIZE_IS_FIXED_X)
    {
        base->size.IsXFixed = *((bool*)&val);
        return true;
    }

    if (type == GuiInstanceBaseAttributeType::SIZE_IS_FIXED_Y)
    {
        base->size.IsYFixed = *((bool*)&val);
        return true;
    }

    if (type == GuiInstanceBaseAttributeType::ID)
    {
        base->id = *((int64_t*)&val);
        return true;
    }

    if (type == GuiInstanceBaseAttributeType::PARENT_ID)
    {
        return false;
        // if (base->parent == NULL)
        //     return false;

        // base->parent->id = *((int64_t*)&val);
        // return true;
    }

    if (type == GuiInstanceBaseAttributeType::IS_HIDDEN)
    {
        base->hidden= *((bool*)&val);
        return true;
    }

    return false;
}

uint64_t GuiInstance::GetBaseComponentAttribute(int64_t id, GuiInstanceBaseAttributeType type)
{
    GuiComponentStuff::BaseComponent* base = GetComponentFromId(id);
    if (base == NULL)
        return 0;

    uint8_t temp[8];
    for (int i = 0; i < 8; i++)
        temp[i] = 0;

    if (type == GuiInstanceBaseAttributeType::POSITION_X)
        *((int32_t*)temp) = base->position.x;
    if (type == GuiInstanceBaseAttributeType::POSITION_Y)
        *((int32_t*)temp) = base->position.y;

    if (type == GuiInstanceBaseAttributeType::SIZE_FIXED_X)
        *((int32_t*)temp) = base->size.FixedX;
    if (type == GuiInstanceBaseAttributeType::SIZE_FIXED_Y)
        *((int32_t*)temp) = base->size.FixedY;
    if (type == GuiInstanceBaseAttributeType::ACTUAL_SIZE_X)
            *((int32_t*)temp) = base->GetActualComponentSize().FixedX;
    if (type == GuiInstanceBaseAttributeType::ACTUAL_SIZE_Y)
            *((int32_t*)temp) = base->GetActualComponentSize().FixedY;
    if (type == GuiInstanceBaseAttributeType::SIZE_SCALED_X)
        *((double*)temp) = base->size.ScaledX;
    if (type == GuiInstanceBaseAttributeType::SIZE_SCALED_Y)
        *((double*)temp) = base->size.ScaledY;
    if (type == GuiInstanceBaseAttributeType::SIZE_IS_FIXED_X)
        *((bool*)temp) = base->size.IsXFixed;
    if (type == GuiInstanceBaseAttributeType::SIZE_IS_FIXED_Y)
        *((bool*)temp) = base->size.IsYFixed;

    if (type == GuiInstanceBaseAttributeType::ID)
        *((int64_t*)temp) = base->id;
    if (type == GuiInstanceBaseAttributeType::PARENT_ID)
        if (base->parent != NULL)
                *((int64_t*)temp) =  base->parent->id;
    if (type == GuiInstanceBaseAttributeType::IS_HIDDEN)
        *((bool*)temp) = base->hidden;

    return *((uint64_t*)temp);
}


bool GuiInstance::ComponentAddChild(int64_t id, GuiComponentStuff::BaseComponent* childComp)
{
    GuiComponentStuff::BaseComponent* parentComp = GetComponentFromId(id);
    //GuiComponentStuff::BaseComponent* childComp = GetComponentFromId(childId);
    if (parentComp == NULL || childComp == NULL)
        return false;
    
    if (parentComp->componentType == GuiComponentStuff::ComponentType::BOX)
    {
        GuiComponentStuff::BoxComponent* box = (GuiComponentStuff::BoxComponent*)parentComp;
        box->children->add(childComp);
        return true;
    }
    if (parentComp->componentType == GuiComponentStuff::ComponentType::SCREEN)
    {
        GuiComponentStuff::ScreenComponent* scr = (GuiComponentStuff::ScreenComponent*)parentComp;
        scr->children->add(childComp);
        return true;
    }

    return false;
}

bool GuiInstance::ComponentRemoveChild(int64_t id, int64_t childId)
{
    GuiComponentStuff::BaseComponent* parentComp = GetComponentFromId(id);
    GuiComponentStuff::BaseComponent* childComp = GetComponentFromId(childId);
    if (parentComp == NULL || childComp == NULL)
        return false;
    
    if (parentComp->componentType == GuiComponentStuff::ComponentType::BOX)
    {
        GuiComponentStuff::BoxComponent* box = (GuiComponentStuff::BoxComponent*)parentComp;
        int indx = box->children->getIndexOf(childComp);
        if (indx == -1)
            return false;
        box->children->removeAt(indx);
        return true;
    }
    if (parentComp->componentType == GuiComponentStuff::ComponentType::SCREEN)
    {
        GuiComponentStuff::ScreenComponent* scr = (GuiComponentStuff::ScreenComponent*)parentComp;
        int indx = scr->children->getIndexOf(childComp);
        if (indx == -1)
            return false;
        scr->children->removeAt(indx);
        return true;
    }


    return false;
}



bool GuiInstance::CreateComponentWithIdAndParent(int64_t id, GuiComponentStuff::ComponentType type, int64_t parentId)
{
    if (GetComponentFromId(id) != NULL)
        return false;
    GuiComponentStuff::BaseComponent* parentComp = GetComponentFromId(parentId);
    if (parentComp == NULL)
    {
        if (screen == NULL)
            return false;
        parentId = screen->id;
        parentComp = screen;
    }
       

    if (type == GuiComponentStuff::ComponentType::NONE)
    {
        return true;
    }

    if (type == GuiComponentStuff::ComponentType::SCREEN)
    {
        GuiComponentStuff::ScreenComponent* scr = new GuiComponentStuff::ScreenComponent(window);
        scr->parent = parentComp;

        allComponents->add(scr);
        return ComponentAddChild(parentId, scr);
    }

    if (type == GuiComponentStuff::ComponentType::BOX)
    {
        GuiComponentStuff::BoxComponent* comp =
        new GuiComponentStuff::BoxComponent(
            parentComp, 
            GuiComponentStuff::ComponentSize(50, 50),
            Colors.tblack
        );
        comp->id = id;

        allComponents->add(comp);
        return ComponentAddChild(parentId, comp);
    }
    if (type == GuiComponentStuff::ComponentType::BUTTON)
    {
        GuiComponentStuff::ButtonComponent* comp =
        new GuiComponentStuff::ButtonComponent(StrCopy(""), 
            Colors.black, Colors.dgray, Colors.white,
            Colors.white, Colors.bgray, Colors.black,
            GuiComponentStuff::ComponentSize(50, 50),
            GuiComponentStuff::Position(0, 0),
            parentComp
        );
        comp->id = id;
        allComponents->add(comp->actualButtonStuff);
        allComponents->add(comp->rectComp);
        allComponents->add(comp->textComp);
        

        allComponents->add(comp);
        return ComponentAddChild(parentId, comp);
    }
    if (type == GuiComponentStuff::ComponentType::RECT)
    {
        GuiComponentStuff::RectangleComponent* comp =
        new GuiComponentStuff::RectangleComponent(
            Colors.black,
            GuiComponentStuff::ComponentSize(50, 50),
            parentComp 
        );
        comp->id = id;

        allComponents->add(comp);
        return ComponentAddChild(parentId, comp);
    }
    if (type == GuiComponentStuff::ComponentType::TEXT)
    {
        GuiComponentStuff::TextComponent* comp =
        new GuiComponentStuff::TextComponent(
            parentComp,
            Colors.white,
            Colors.black,
            StrCopy(""),
            GuiComponentStuff::Position(0, 0)
        );
        comp->id = id;

        allComponents->add(comp);
        return ComponentAddChild(parentId, comp);
    }
    if (type == GuiComponentStuff::ComponentType::TEXTFIELD)
    {
        GuiComponentStuff::TextFieldComponent* comp =
        new GuiComponentStuff::TextFieldComponent(
            Colors.black,
            Colors.white,
            GuiComponentStuff::ComponentSize(50, 50),
            GuiComponentStuff::Position(0, 0),
            parentComp
        );
        comp->id = id;

        allComponents->add(comp->actualTextFieldStuff);
        allComponents->add(comp->rectComp);
        allComponents->add(comp->textComp);

        allComponents->add(comp);
        return ComponentAddChild(parentId, comp);
    }

    return false;
}