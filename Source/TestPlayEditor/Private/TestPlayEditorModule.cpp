#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformFileManager.h"
#include "IDesktopPlatform.h"
#include "Interfaces/IMainFrameModule.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"
#include "Stats/Stats.h"
#include "TestPlayRunner.h"
#include "TestPlaySpec.h"
#include "TickableEditorObject.h"
#include "ToolMenus.h"

DEFINE_LOG_CATEGORY_STATIC(LogTestPlayEditor, Log, All);

class FTestPlayEditorModule : public IModuleInterface, public FTickableEditorObject
{
public:
	virtual void StartupModule() override
	{
		FCoreDelegates::OnPostEngineInit.AddRaw(this, &FTestPlayEditorModule::HandlePostEngineInit);
		FEditorDelegates::PostPIEStarted.AddRaw(this, &FTestPlayEditorModule::HandlePostPIEStarted);
		FEditorDelegates::EndPIE.AddRaw(this, &FTestPlayEditorModule::HandleEndPIE);

		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FTestPlayEditorModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		FCoreDelegates::OnPostEngineInit.RemoveAll(this);
		FEditorDelegates::PostPIEStarted.RemoveAll(this);
		FEditorDelegates::EndPIE.RemoveAll(this);
	}

	virtual void Tick(float DeltaTime) override
	{
		if (bRunSpecNextTick)
		{
			DeferredRunDelaySeconds -= DeltaTime;
			if (DeferredRunDelaySeconds <= 0.0f)
			{
				bRunSpecNextTick = false;
				RunSpec(DeferredSpecPath, DeferredExitOnComplete);
			}
		}

		if (Runner.IsValid() && !Runner->IsComplete())
		{
			Runner->Tick(DeltaTime);
		}

		if (Runner.IsValid() && Runner->IsComplete())
		{
			FinishRun();
		}

		if (bExitAfterPIE && GEditor && !GEditor->PlayWorld)
		{
			FPlatformMisc::RequestExit(false);
		}
	}

	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FTestPlayEditorModule, STATGROUP_Tickables);
	}

	virtual bool IsTickable() const override
	{
		return bRunSpecNextTick || Runner.IsValid() || bExitAfterPIE;
	}

private:
	void HandlePostEngineInit()
	{
		FString SpecPath;
		if (FParse::Value(FCommandLine::Get(), TEXT("TestPlayRun="), SpecPath))
		{
			FParse::Value(FCommandLine::Get(), TEXT("TestPlayResult="), CommandLineResultPath);
			DeferredSpecPath = SpecPath;
			DeferredExitOnComplete = FParse::Param(FCommandLine::Get(), TEXT("TestPlayExitOnComplete"));
			DeferredRunDelaySeconds = 0.5f;
			bRunSpecNextTick = true;
		}
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
		FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("TestPlay"));
		Section.AddMenuEntry(
			TEXT("RunTestPlaySpec"),
			FText::FromString(TEXT("Run TestPlay Spec...")),
			FText::FromString(TEXT("Run a JSON TestPlay spec in PIE.")),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FTestPlayEditorModule::OpenSpecDialog)));
	}

	void OpenSpecDialog()
	{
		IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
		if (DesktopPlatform == nullptr)
		{
			return;
		}

		const void* ParentWindowHandle = nullptr;
		if (FSlateApplication::IsInitialized())
		{
			ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		}

		TArray<FString> FileNames;
		const bool bOpened = DesktopPlatform->OpenFileDialog(
			ParentWindowHandle,
			TEXT("Select TestPlay Spec"),
			FPaths::ProjectDir(),
			TEXT(""),
			TEXT("JSON files (*.json)|*.json"),
			EFileDialogFlags::None,
			FileNames);

		if (bOpened && FileNames.Num() > 0)
		{
			RunSpec(FileNames[0], false);
		}
	}

	void RunSpec(const FString& SpecPath, bool bInExitOnComplete)
	{
		if (Runner.IsValid())
		{
			UE_LOG(LogTestPlayEditor, Warning, TEXT("TestPlay run is already active."));
			return;
		}

		FString Error;
		if (!FTestPlaySpec::LoadFromFile(SpecPath, PendingSpec, Error))
		{
			UE_LOG(LogTestPlayEditor, Error, TEXT("TESTPLAY_RESULT: FAILED - %s"), *Error);
			WriteLoadFailureResult(SpecPath, Error);
			if (bInExitOnComplete)
			{
				FPlatformMisc::RequestExit(false);
			}
			return;
		}

		bExitOnComplete = bInExitOnComplete;
		ResultPath = ResolveResultPath(SpecPath, PendingSpec.Name);
		UE_LOG(LogTestPlayEditor, Display, TEXT("Running TestPlay spec: %s"), *SpecPath);

		if (!PendingSpec.Map.IsEmpty())
		{
			FString MapFilename = PendingSpec.Map;
			if (FPackageName::IsValidLongPackageName(PendingSpec.Map))
			{
				MapFilename = FPackageName::LongPackageNameToFilename(PendingSpec.Map, FPackageName::GetMapPackageExtension());
			}

			if (!FEditorFileUtils::LoadMap(MapFilename, false, true))
			{
				Error = FString::Printf(TEXT("Could not load map: %s"), *PendingSpec.Map);
				UE_LOG(LogTestPlayEditor, Error, TEXT("TESTPLAY_RESULT: FAILED - %s"), *Error);
				WriteLoadFailureResult(SpecPath, Error);
				if (bInExitOnComplete)
				{
					FPlatformMisc::RequestExit(false);
				}
				return;
			}
		}

		FRequestPlaySessionParams Params;
		Params.WorldType = EPlaySessionWorldType::PlayInEditor;
		GEditor->RequestPlaySession(Params);
	}

	void HandlePostPIEStarted(bool bIsSimulating)
	{
		if (PendingSpec.Steps.Num() == 0)
		{
			return;
		}

		UWorld* PIEWorld = FindPIEWorld();
		if (PIEWorld == nullptr)
		{
			UE_LOG(LogTestPlayEditor, Error, TEXT("TESTPLAY_RESULT: FAILED - Could not find PIE world."));
			return;
		}

		Runner = MakeShared<FTestPlayRunner>(PIEWorld, PendingSpec);
		Runner->Start();
	}

	void HandleEndPIE(bool bIsSimulating)
	{
		if (Runner.IsValid() && !Runner->IsComplete())
		{
			UE_LOG(LogTestPlayEditor, Warning, TEXT("TestPlay PIE session ended before the runner completed."));
			Runner.Reset();
		}

		if (bExitOnComplete)
		{
			bExitAfterPIE = true;
		}
	}

	void FinishRun()
	{
		const FTestPlayResult Result = Runner->GetResult();
		FString Error;
		if (!Result.WriteToFile(ResultPath, Error))
		{
			UE_LOG(LogTestPlayEditor, Error, TEXT("Could not write TestPlay result: %s"), *Error);
		}
		else
		{
			UE_LOG(LogTestPlayEditor, Display, TEXT("Wrote TestPlay result: %s"), *ResultPath);
		}

		Runner.Reset();
		PendingSpec = FTestPlaySpec();

		if (GEditor && GEditor->PlayWorld)
		{
			GEditor->RequestEndPlayMap();
		}
		else if (bExitOnComplete)
		{
			FPlatformMisc::RequestExit(false);
		}
	}

	UWorld* FindPIEWorld() const
	{
		if (!GEditor)
		{
			return nullptr;
		}

		for (const FWorldContext& WorldContext : GEditor->GetWorldContexts())
		{
			if (WorldContext.WorldType == EWorldType::PIE)
			{
				return WorldContext.World();
			}
		}

		return nullptr;
	}

	FString ResolveResultPath(const FString& SpecPath, const FString& SuiteName) const
	{
		if (!CommandLineResultPath.IsEmpty())
		{
			return CommandLineResultPath;
		}

		const FString SafeSuiteName = SuiteName.IsEmpty() ? FPaths::GetBaseFilename(SpecPath) : SuiteName;
		return FPaths::ProjectSavedDir() / TEXT("TestPlay/Results") / FString::Printf(TEXT("%s.latest.json"), *SafeSuiteName);
	}

	void WriteLoadFailureResult(const FString& SpecPath, const FString& Error) const
	{
		FTestPlayResult LoadFailure;
		LoadFailure.SuiteName = FPaths::GetBaseFilename(SpecPath);
		LoadFailure.bSuccess = false;
		LoadFailure.FailedStep = INDEX_NONE;
		LoadFailure.Error = Error;
		LoadFailure.AddLog(Error);

		FString WriteError;
		LoadFailure.WriteToFile(ResolveResultPath(SpecPath, LoadFailure.SuiteName), WriteError);
	}

	FTestPlaySpec PendingSpec;
	FString ResultPath;
	FString CommandLineResultPath;
	FString DeferredSpecPath;
	TSharedPtr<FTestPlayRunner> Runner;
	bool bExitOnComplete = false;
	bool bExitAfterPIE = false;
	bool bRunSpecNextTick = false;
	bool DeferredExitOnComplete = false;
	float DeferredRunDelaySeconds = 0.0f;
};

IMPLEMENT_MODULE(FTestPlayEditorModule, TestPlayEditor)
