// Copyright 2015-2020 Piperift. All Rights Reserved.

#pragma once

// Integrate Paper2D actions associated with existing engine types (e.g., Texture2D) into the content browser
class FBlueprintRetargetContentBrowserExtensions
{
public:
	static void InstallHooks();
	static void RemoveHooks();
};