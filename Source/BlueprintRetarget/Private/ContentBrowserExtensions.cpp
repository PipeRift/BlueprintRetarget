// Copyright 2015-2020 Piperift. All Rights Reserved.

#include "ContentBrowserExtensions.h"

#include <CoreGlobals.h>
#include <AssetToolsModule.h>
#include <AssetRegistryModule.h>
#include <ContentBrowserModule.h>
#include <IContentBrowserSingleton.h>
#include <EditorStyleSet.h>
#include <Styling/CoreStyle.h>
#include <Framework/Commands/UIAction.h>
#include <Framework/Commands/UICommandInfo.h>
#include <Framework/MultiBox/MultiBoxBuilder.h>
#include <Framework/Notifications/NotificationManager.h>
#include <Widgets/Notifications/SNotificationList.h>
#include <Misc/ConfigCacheIni.h>
#include <Misc/MessageDialog.h>
#include <Dialogs/Dialogs.h>

#include <ScopedTransaction.h>
#include <Engine/Blueprint.h>
#include <ClassViewerModule.h>
#include <ClassViewerFilter.h>
#include <Kismet2/SClassPickerDialog.h>
#include <Kismet2/KismetEditorUtilities.h>
#include <Kismet2/CompilerResultsLog.h>
#include <Kismet2/BlueprintEditorUtils.h>

#include <GameFramework/Actor.h>
#include <Components/ActorComponent.h>
#include <Animation/AnimBlueprint.h>
#include <Animation/AnimInstance.h>
#include <Engine/LevelScriptActor.h>
#include <Engine/SCS_Node.h>
#include <Engine/SimpleConstructionScript.h>


#define LOCTEXT_NAMESPACE "BlueprintRetarget"


class FRetargetBlueprintFilter : public IClassViewerFilter
{
public:
	/** All children of these classes will be included unless filtered out by another setting. */
	TSet< const UClass* > AllowedChildrenOfClasses;

	/** Classes to not allow any children of into the Class Viewer/Picker. */
	TSet< const UClass* > DisallowedChildrenOfClasses;

	/** Classes to never show in this class viewer. */
	TSet< const UClass* > DisallowedClasses;

	/** Will limit the results to only native classes */
	bool bShowNativeOnly;

	FRetargetBlueprintFilter()
		: bShowNativeOnly(false)
	{}

	virtual bool IsClassAllowed(const FClassViewerInitializationOptions& InInitOptions, const UClass* InClass, TSharedRef< FClassViewerFilterFuncs > InFilterFuncs) override
	{
		// If it appears on the allowed child-of classes list (or there is nothing on that list)
		//		AND it is NOT on the disallowed child-of classes list
		//		AND it is NOT on the disallowed classes list
		return InFilterFuncs->IfInChildOfClassesSet(AllowedChildrenOfClasses, InClass) != EFilterReturn::Failed &&
			InFilterFuncs->IfInChildOfClassesSet(DisallowedChildrenOfClasses, InClass) != EFilterReturn::Passed &&
			InFilterFuncs->IfInClassesSet(DisallowedClasses, InClass) != EFilterReturn::Passed &&
			!InClass->HasAnyClassFlags(CLASS_Deprecated) &&
			((bShowNativeOnly && InClass->HasAnyClassFlags(CLASS_Native)) || !bShowNativeOnly);
	}

	virtual bool IsUnloadedClassAllowed(const FClassViewerInitializationOptions& InInitOptions, const TSharedRef< const IUnloadedBlueprintData > InUnloadedClassData, TSharedRef< FClassViewerFilterFuncs > InFilterFuncs) override
	{
		// If it appears on the allowed child-of classes list (or there is nothing on that list)
		//		AND it is NOT on the disallowed child-of classes list
		//		AND it is NOT on the disallowed classes list
		return InFilterFuncs->IfInChildOfClassesSet(AllowedChildrenOfClasses, InUnloadedClassData) != EFilterReturn::Failed &&
			InFilterFuncs->IfInChildOfClassesSet(DisallowedChildrenOfClasses, InUnloadedClassData) != EFilterReturn::Passed &&
			InFilterFuncs->IfInClassesSet(DisallowedClasses, InUnloadedClassData) != EFilterReturn::Passed &&
			!InUnloadedClassData->HasAnyClassFlags(CLASS_Deprecated) &&
			((bShowNativeOnly && InUnloadedClassData->HasAnyClassFlags(CLASS_Native)) || !bShowNativeOnly);
	}
};


//////////////////////////////////////////////////////////////////////////

FContentBrowserMenuExtender_SelectedAssets ContentBrowserExtenderDelegate;
FDelegateHandle ContentBrowserExtenderDelegateHandle;


//////////////////////////////////////////////////////////////////////////
// FContentBrowserSelectedAssetExtensionBase

struct FContentBrowserSelectedAssetExtensionBase
{
public:
	TArray<struct FAssetData> SelectedAssets;

public:
	virtual void Execute() {}
	virtual ~FContentBrowserSelectedAssetExtensionBase() {}
};


//////////////////////////////////////////////////////////////////////////
// FRetargetClassExtension

struct FRetargetClassExtension : public FContentBrowserSelectedAssetExtensionBase
{
	FRetargetClassExtension()
	{}

	virtual void Execute() override
	{
		// Only cache Blueprint assets
		TArray<UBlueprint*> BPs;
		for (auto AssetIt = SelectedAssets.CreateConstIterator(); AssetIt; ++AssetIt)
		{
			const FAssetData& AssetData = *AssetIt;
			if (TAssetPtr<UBlueprint> BP = Cast<UBlueprint>(AssetData.GetAsset()))
			{
				BPs.Add(BP.Get());
			}
		}

		const FText WarningTitle = LOCTEXT("RetargetWarningTitle", "WARNING");
		EAppReturnType::Type Result = FMessageDialog::Open(EAppMsgType::OkCancel, EAppReturnType::Ok,
			LOCTEXT("RetargetWarning", "This tool is ONLY intended to fix missing or invalid blueprint parents.\n\nDo not try to reparent a working blueprint with it. Assigning parent classes that changed or are unrelated may corrupt your blueprint."),
			&WarningTitle
		);

		if (Result == EAppReturnType::Cancel)
			return;

		// Assign a new parent class
		UClass* ChosenClass{ SelectClass(BPs) };

		if (ChosenClass)
		{
			const FScopedTransaction Transaction(LOCTEXT("RetargetBlueprintParents", "Retarget Blueprint parents"));
			for (auto* BP : BPs)
			{
				ReparentBlueprint(BP, ChosenClass);
			}
		}
	}

	UClass* SelectClass(const TArray<UBlueprint*>& BPs)
	{
		// Class picker options
		FClassViewerInitializationOptions Options;
		{
			Options.Mode = EClassViewerMode::ClassPicker;
			Options.DisplayMode = EClassViewerDisplayMode::TreeView;
			Options.bShowObjectRootClass = true;

			Options.bIsBlueprintBaseOnly = true; // Only want blueprint base classes
			Options.bShowUnloadedBlueprints = true;

			Options.ClassFilter = PrepareFilter(BPs);
		}

		// Temporally hide custom picker from ClassPicker
		bool bLastExpandCustomClasses = true;
		GConfig->GetBool(TEXT("/Script/UnrealEd.UnrealEdOptions"), TEXT("bExpandClassPickerDefaultClassList"), bLastExpandCustomClasses, GEditorIni);
		GConfig->SetBool(TEXT("/Script/UnrealEd.UnrealEdOptions"), TEXT("bExpandClassPickerDefaultClassList"), false, GEditorIni);

		UClass* ChosenClass = nullptr;
		const FText TitleText = LOCTEXT("ClassPickerTitle", "Pick Parent Class");
		const bool bPressedOk = SClassPickerDialog::PickClass(TitleText, Options, ChosenClass, UBlueprint::StaticClass());

		// Restore custom picker config value
		GConfig->SetBool(TEXT("/Script/UnrealEd.UnrealEdOptions"), TEXT("bExpandClassPickerDefaultClassList"), bLastExpandCustomClasses, GEditorIni);


		return bPressedOk? ChosenClass : nullptr;
	}

	TSharedPtr<FRetargetBlueprintFilter> PrepareFilter(const TArray<UBlueprint*> BPs)
	{
		// Gather BP information
		TArray<const UClass*> BlueprintClasses{};
		bool bHasParent = true;
		bool bIsActor = false;
		bool bIsAnimBlueprint = false;
		bool bIsLevelScriptActor = false;
		bool bIsComponentBlueprint = false;
		for (auto BlueprintIt = BPs.CreateConstIterator(); (!bIsActor && !bIsAnimBlueprint) && BlueprintIt; ++BlueprintIt)
		{
			const UBlueprint* Blueprint = *BlueprintIt;

			bIsAnimBlueprint |= Blueprint->IsA(UAnimBlueprint::StaticClass());

			if (Blueprint->ParentClass)
			{
				bIsActor |= Blueprint->ParentClass->IsChildOf(AActor::StaticClass());
				bIsLevelScriptActor |= Blueprint->ParentClass->IsChildOf(ALevelScriptActor::StaticClass());
				bIsComponentBlueprint |= Blueprint->ParentClass->IsChildOf(UActorComponent::StaticClass());
				if (Blueprint->GeneratedClass)
				{
					BlueprintClasses.Add(Blueprint->GeneratedClass);
				}
			}
			else
			{
				bHasParent = false;
			}
		}


		TSharedPtr<FRetargetBlueprintFilter> Filter = MakeShared<FRetargetBlueprintFilter>();

		Filter->DisallowedChildrenOfClasses.Add(UInterface::StaticClass());
		Filter->DisallowedChildrenOfClasses.Append(BlueprintClasses); // Can't re-parent to child BPs
		for (UBlueprint* BP : BPs)
		{
			BP->GetReparentingRules(Filter->AllowedChildrenOfClasses, Filter->DisallowedChildrenOfClasses);
		}

		if (bIsActor)
		{
			if (bIsLevelScriptActor)
			{
				// Don't allow conversion outside of the LevelScriptActor hierarchy
				Filter->AllowedChildrenOfClasses.Add(ALevelScriptActor::StaticClass());
				Filter->bShowNativeOnly = true;
			}
			else
			{
				// Don't allow conversion outside of the Actor hierarchy
				Filter->AllowedChildrenOfClasses.Add(AActor::StaticClass());

				// Don't allow non-LevelScriptActor->LevelScriptActor conversion
				Filter->DisallowedChildrenOfClasses.Add(ALevelScriptActor::StaticClass());
			}
		}
		else if (bIsAnimBlueprint)
		{
			// If it's an anim blueprint, do not allow conversion to non anim
			Filter->AllowedChildrenOfClasses.Add(UAnimInstance::StaticClass());
		}
		else if (bIsComponentBlueprint)
		{
			// If it is a component blueprint, only allow classes under and including UActorComponent
			Filter->AllowedChildrenOfClasses.Add(UActorComponent::StaticClass());
		}
		else if(bHasParent)
		{
			Filter->DisallowedChildrenOfClasses.Add(AActor::StaticClass());
		}

		for (const UBlueprint* BP : BPs)
		{
			// don't allow making me my own parent!
			if (BP->GeneratedClass)
			{
				Filter->DisallowedClasses.Add(BP->GeneratedClass);
			}
		}
		return Filter;
	}

	void ReparentBlueprint(UBlueprint* Blueprint, UClass* ChosenClass) {
		check(Blueprint);

		if ((Blueprint != nullptr) && (ChosenClass != nullptr) && (ChosenClass != Blueprint->ParentClass))
		{
			// Notify user, about common interfaces
			bool bReparent = true;
			{
				FString CommonInterfacesNames;
				for (const FBPInterfaceDescription& InterdaceDesc : Blueprint->ImplementedInterfaces)
				{
					if (ChosenClass->ImplementsInterface(*InterdaceDesc.Interface))
					{
						CommonInterfacesNames += InterdaceDesc.Interface->GetName();
						CommonInterfacesNames += TCHAR('\n');
					}
				}
				if (!CommonInterfacesNames.IsEmpty())
				{
					const FText Title = LOCTEXT("CommonInterfacesTitle", "Common interfaces");
					const FText Message = FText::Format(
						LOCTEXT("ReparentWarning_InterfacesImplemented", "Following interfaces are already implemented. Continue reparenting? \n {0}"),
						FText::FromString(CommonInterfacesNames));

					FSuppressableWarningDialog::FSetupInfo Info(Message, Title, "Warning_CommonInterfacesWhileReparenting");
					Info.ConfirmText = LOCTEXT("ReparentYesButton", "Reparent");
					Info.CancelText = LOCTEXT("ReparentNoButton", "Cancel");

					if (FSuppressableWarningDialog(Info).ShowModal() == FSuppressableWarningDialog::Cancel)
					{
						bReparent = false;
					}
				}
			}

			// If the chosen class differs hierarchically from the current class, warn that there may be data loss
			if (bReparent && (!Blueprint->ParentClass || !ChosenClass->GetDefaultObject()->IsA(Blueprint->ParentClass)))
			{
				const FText Title   = LOCTEXT("ReparentTitle", "Reparent Blueprint");
				const FText Message = LOCTEXT("ReparentWarning", "Reparenting this blueprint may cause data loss.  Continue reparenting?");

				// Warn the user that this may result in data loss
				FSuppressableWarningDialog::FSetupInfo Info(Message, Title, "Warning_ReparentTitle");
				Info.ConfirmText = LOCTEXT("ReparentYesButton", "Reparent");
				Info.CancelText = LOCTEXT("ReparentNoButton", "Cancel");
				Info.CheckBoxText = FText::GetEmpty();	// not suppressible

				if (FSuppressableWarningDialog(Info).ShowModal() == FSuppressableWarningDialog::Cancel)
				{
					bReparent = false;
				}
			}

			if (bReparent)
			{
				UE_LOG(LogBlueprintReparent, Warning, TEXT("Reparenting blueprint %s from %s to %s..."), *Blueprint->GetFullName(), Blueprint->ParentClass ? *Blueprint->ParentClass->GetName() : TEXT("[None]"), *ChosenClass->GetName());

				UClass* OldParentClass = Blueprint->ParentClass;
				Blueprint->ParentClass = ChosenClass;

				// Ensure that the Blueprint is up-to-date (valid SCS etc.) before compiling
				EnsureBlueprintIsUpToDate(Blueprint);
				FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

				Compile(Blueprint);

				// Ensure that the Blueprint is up-to-date (valid SCS etc.) after compiling (new parent class)
				EnsureBlueprintIsUpToDate(Blueprint);

				if (Blueprint->NativizationFlag != EBlueprintNativizationFlag::Disabled)
				{
					UBlueprint* ParentBlueprint = UBlueprint::GetBlueprintFromClass(ChosenClass);
					if (ParentBlueprint && ParentBlueprint->NativizationFlag == EBlueprintNativizationFlag::Disabled)
					{
						ParentBlueprint->NativizationFlag = EBlueprintNativizationFlag::Dependency;

						FNotificationInfo Warning(FText::Format(
							LOCTEXT("InterfaceFlaggedForNativization", "{0} flagged for nativization (as a required dependency)."),
							FText::FromName(ParentBlueprint->GetFName())
						));
						Warning.ExpireDuration = 5.0f;
						Warning.bFireAndForget = true;
						Warning.Image = FCoreStyle::Get().GetBrush(TEXT("MessageLog.Warning"));
						FSlateNotificationManager::Get().AddNotification(Warning);
					}
				}

				/*if (SCSEditor.IsValid())
				{
					SCSEditor->UpdateTree();
				}*/
			}
		}

		FSlateApplication::Get().DismissAllMenus();
	}

	void Compile(UBlueprint* Blueprint) {
		check(Blueprint);

		FCompilerResultsLog LogResults;
		LogResults.SetSourcePath(Blueprint->GetPathName());
		LogResults.BeginEvent(TEXT("Compile"));
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &LogResults);

		LogResults.EndEvent();
	}

	void EnsureBlueprintIsUpToDate(UBlueprint* Blueprint) {

		// Purge any nullptr graphs
		FBlueprintEditorUtils::PurgeNullGraphs(Blueprint);

		// Make sure the blueprint is cosmetically up to date
		FKismetEditorUtilities::UpgradeCosmeticallyStaleBlueprint(Blueprint);

		if (FBlueprintEditorUtils::SupportsConstructionScript(Blueprint))
		{
			// If we don't have an SCS yet, make it
			if (Blueprint->GeneratedClass && !Blueprint->SimpleConstructionScript)
			{
				Blueprint->SimpleConstructionScript = NewObject<USimpleConstructionScript>(Blueprint->GeneratedClass);
				Blueprint->SimpleConstructionScript->SetFlags(RF_Transactional);
			}

			// If we should have a UCS but don't yet, make it
			if (!FBlueprintEditorUtils::FindUserConstructionScript(Blueprint))
			{
				UEdGraph* UCSGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, UEdGraphSchema_K2::FN_UserConstructionScript, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
				FBlueprintEditorUtils::AddFunctionGraph(Blueprint, UCSGraph, /*bIsUserCreated=*/ false, AActor::StaticClass());
				UCSGraph->bAllowDeletion = false;
			}

			// Check to see if we have gained a component from our parent (that would require us removing our scene root)
			// (or lost one, which requires adding one)
			if (Blueprint->SimpleConstructionScript != nullptr)
			{
				Blueprint->SimpleConstructionScript->ValidateSceneRootNodes();
			}
		}
		else
		{
			// If we have an SCS but don't support it, then we remove it
			if (Blueprint->SimpleConstructionScript)
			{
				// Remove any SCS variable nodes
				for (USCS_Node* SCS_Node : Blueprint->SimpleConstructionScript->GetAllNodes())
				{
					if (SCS_Node)
					{
						FBlueprintEditorUtils::RemoveVariableNodes(Blueprint, SCS_Node->GetVariableName());
					}
				}

				// Remove the SCS object reference
				Blueprint->SimpleConstructionScript = nullptr;

				// Mark the Blueprint as having been structurally modified
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			}
		}

		// Make sure that this blueprint is up-to-date with regards to its parent functions
		FBlueprintEditorUtils::ConformCallsToParentFunctions(Blueprint);

		// Make sure that this blueprint is up-to-date with regards to its implemented events
		FBlueprintEditorUtils::ConformImplementedEvents(Blueprint);

		// Make sure that this blueprint is up-to-date with regards to its implemented interfaces
		FBlueprintEditorUtils::ConformImplementedInterfaces(Blueprint);

		// Update old composite nodes(can't do this in PostLoad)
		FBlueprintEditorUtils::UpdateOutOfDateCompositeNodes(Blueprint);

		// Update any nodes which might have dropped their RF_Transactional flag due to copy-n-paste issues
		FBlueprintEditorUtils::UpdateTransactionalFlags(Blueprint);
	}
};


//////////////////////////////////////////////////////////////////////////
// FBlueprintRetargetContentBrowserExtensions_Impl

class FBlueprintRetargetContentBrowserExtensions_Impl
{
public:
	static void ExecuteSelectedContentFunctor(TSharedPtr<FContentBrowserSelectedAssetExtensionBase> SelectedAssetFunctor)
	{
		SelectedAssetFunctor->Execute();
	}

	static void CreateSubMenu(FMenuBuilder& MenuBuilder, TArray<FAssetData> SelectedAssets)
	{
		TSharedPtr<FRetargetClassExtension> ICreatorFunctor = MakeShareable(new FRetargetClassExtension());
		ICreatorFunctor->SelectedAssets = SelectedAssets;

		FUIAction Action_RetargetClass(FExecuteAction::CreateStatic(
			&FBlueprintRetargetContentBrowserExtensions_Impl::ExecuteSelectedContentFunctor,
			StaticCastSharedPtr<FContentBrowserSelectedAssetExtensionBase>(ICreatorFunctor)
		));

		MenuBuilder.AddMenuEntry(
			LOCTEXT("RetargetClass", "Retarget invalid parent"),
			LOCTEXT("RetargetClass_Tooltip", "Reparents a blueprint's parent class (Useful when parent class is missing or invalid)"),
			FSlateIcon(FEditorStyle::GetStyleSetName(), "ClassIcon.Note"),
			Action_RetargetClass,
			NAME_None,
			EUserInterfaceActionType::Button);
	}

	static TSharedRef<FExtender> OnExtendContentBrowserAssetSelectionMenu(const TArray<FAssetData>& SelectedAssets)
	{
		// Run through the assets to determine if any meet our criteria
		bool bAllInvalidBlueprint = SelectedAssets.Num() > 0;
		for (auto AssetIt = SelectedAssets.CreateConstIterator(); AssetIt; ++AssetIt)
		{
			bAllInvalidBlueprint &= IsInvalidBlueprint(Cast<UBlueprint>(AssetIt->GetAsset()));
		}

		TSharedRef<FExtender> Extender(new FExtender());

		if (bAllInvalidBlueprint)
		{
			// Add the sprite actions sub-menu extender
			Extender->AddMenuExtension(
				"GetAssetActions",
				EExtensionHook::After,
				nullptr,
				FMenuExtensionDelegate::CreateStatic(&FBlueprintRetargetContentBrowserExtensions_Impl::CreateSubMenu, SelectedAssets));
		}

		return Extender;
	}

	static TArray<FContentBrowserMenuExtender_SelectedAssets>& GetExtenderDelegates()
	{
		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		return ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
	}

	static bool IsInvalidBlueprint(const TAssetPtr<UBlueprint>& BP) {
		return BP && (!BP->SkeletonGeneratedClass || !BP->GeneratedClass);
	}
};


//////////////////////////////////////////////////////////////////////////
// FBlueprintRetargetContentBrowserExtensions

void FBlueprintRetargetContentBrowserExtensions::InstallHooks()
{
	ContentBrowserExtenderDelegate = FContentBrowserMenuExtender_SelectedAssets::CreateStatic(&FBlueprintRetargetContentBrowserExtensions_Impl::OnExtendContentBrowserAssetSelectionMenu);

	TArray<FContentBrowserMenuExtender_SelectedAssets>& CBMenuExtenderDelegates = FBlueprintRetargetContentBrowserExtensions_Impl::GetExtenderDelegates();
	CBMenuExtenderDelegates.Add(ContentBrowserExtenderDelegate);
	ContentBrowserExtenderDelegateHandle = CBMenuExtenderDelegates.Last().GetHandle();
}

void FBlueprintRetargetContentBrowserExtensions::RemoveHooks()
{
	TArray<FContentBrowserMenuExtender_SelectedAssets>& CBMenuExtenderDelegates = FBlueprintRetargetContentBrowserExtensions_Impl::GetExtenderDelegates();
	CBMenuExtenderDelegates.RemoveAll([](const FContentBrowserMenuExtender_SelectedAssets& Delegate) { return Delegate.GetHandle() == ContentBrowserExtenderDelegateHandle; });
}

//////////////////////////////////////////////////////////////////////////

#undef LOCTEXT_NAMESPACE
