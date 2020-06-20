// Copyright 2015-2020 Piperift. All Rights Reserved.

#include "BlueprintRetarget.h"
#include "ContentBrowserExtensions.h"


#define LOCTEXT_NAMESPACE "FBlueprintRetargetModule"

DEFINE_LOG_CATEGORY(LogBlueprintReparent);


static const FName BlueprintRetargetTabName("BlueprintRetarget");

void FBlueprintRetargetModule::StartupModule()
{
	FBlueprintRetargetContentBrowserExtensions::InstallHooks();
}

void FBlueprintRetargetModule::ShutdownModule()
{
	FBlueprintRetargetContentBrowserExtensions::RemoveHooks();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBlueprintRetargetModule, BlueprintRetarget)