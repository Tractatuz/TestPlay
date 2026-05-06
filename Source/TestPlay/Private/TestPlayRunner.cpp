#include "TestPlayRunner.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "EngineUtils.h"
#include "EnhancedPlayerInput.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "Input/Events.h"
#include "TestPlayLog.h"
#include "UObject/UObjectIterator.h"

FTestPlayRunner::FTestPlayRunner(UWorld* InWorld, const FTestPlaySpec& InSpec)
	: World(InWorld)
	, Spec(InSpec)
{
	Result.SuiteName = Spec.Name;
}

void FTestPlayRunner::Start()
{
	ElapsedSeconds = 0.0f;
	StepElapsedSeconds = 0.0f;
	CurrentStepIndex = 0;
	bStepStarted = false;
	bComplete = false;
	Result.AddLog(FString::Printf(TEXT("Started TestPlay spec '%s' with %d step(s)."), *Spec.Name, Spec.Steps.Num()));
}

void FTestPlayRunner::Tick(float DeltaSeconds)
{
	if (bComplete)
	{
		return;
	}

	ElapsedSeconds += DeltaSeconds;
	if (ElapsedSeconds > Spec.Timeout)
	{
		FailCurrentStep(FString::Printf(TEXT("Spec timed out after %.2f seconds."), Spec.Timeout));
		return;
	}

	if (CurrentStepIndex >= Spec.Steps.Num())
	{
		CompleteRunner(true);
		return;
	}

	if (TickCurrentStep(DeltaSeconds))
	{
		++CurrentStepIndex;
		StepElapsedSeconds = 0.0f;
		bStepStarted = false;
	}
}

bool FTestPlayRunner::TickCurrentStep(float DeltaSeconds)
{
	const FTestPlayStep& Step = Spec.Steps[CurrentStepIndex];
	FString Error;

	if (!bStepStarted)
	{
		Result.AddLog(FString::Printf(TEXT("Step %d started: %s"), CurrentStepIndex, *GetStepLabel(Step)));
		bStepStarted = true;
	}

	StepElapsedSeconds += DeltaSeconds;

	bool bStepComplete = false;
	if (Step.Type.Equals(TEXT("wait"), ESearchCase::IgnoreCase))
	{
		bStepComplete = TickWaitStep(Step, DeltaSeconds, Error);
	}
	else if (Step.Type.Equals(TEXT("waitForActor"), ESearchCase::IgnoreCase))
	{
		bStepComplete = TickWaitForActorStep(Step, DeltaSeconds, Error);
	}
	else if (Step.Type.Equals(TEXT("waitForWidget"), ESearchCase::IgnoreCase))
	{
		bStepComplete = TickWaitForWidgetStep(Step, DeltaSeconds, Error);
	}
	else if (Step.Type.Equals(TEXT("inputAction"), ESearchCase::IgnoreCase))
	{
		bStepComplete = TickInputActionStep(Step, DeltaSeconds, Error);
	}
	else
	{
		bStepComplete = ExecuteInstantStep(Step, Error);
	}

	if (!Error.IsEmpty())
	{
		FailCurrentStep(Error);
		return false;
	}

	if (bStepComplete)
	{
		Result.AddLog(FString::Printf(TEXT("Step %d completed: %s"), CurrentStepIndex, *GetStepLabel(Step)));
	}

	return bStepComplete;
}

bool FTestPlayRunner::ExecuteInstantStep(const FTestPlayStep& Step, FString& OutError)
{
	if (Step.Type.Equals(TEXT("assertActorExists"), ESearchCase::IgnoreCase))
	{
		if (FindActor(Step.Data) == nullptr)
		{
			OutError = TEXT("Expected actor was not found.");
			return false;
		}

		return true;
	}

	if (Step.Type.Equals(TEXT("assertActorDistance"), ESearchCase::IgnoreCase))
	{
		AActor* Actor = FindActor(Step.Data);
		const FString TargetTag = Step.Data->GetStringField(TEXT("targetTag"));
		AActor* Target = FindActorByTag(FName(*TargetTag));
		if (Actor == nullptr || Target == nullptr)
		{
			OutError = TEXT("Could not find actor or target actor for distance assertion.");
			return false;
		}

		const float Distance = FVector::Distance(Actor->GetActorLocation(), Target->GetActorLocation());
		const float GreaterThan = GetNumberField(Step.Data, TEXT("greaterThan"), -FLT_MAX);
		const float LessThan = GetNumberField(Step.Data, TEXT("lessThan"), FLT_MAX);
		if (Distance <= GreaterThan || Distance >= LessThan)
		{
			OutError = FString::Printf(TEXT("Distance assertion failed. Distance %.2f was not in range (%.2f, %.2f)."), Distance, GreaterThan, LessThan);
			return false;
		}

		return true;
	}

	if (Step.Type.Equals(TEXT("assertActorLocation"), ESearchCase::IgnoreCase))
	{
		AActor* Actor = FindActor(Step.Data);
		FVector ExpectedLocation;
		if (Actor == nullptr || !ReadVector(Step.Data, TEXT("location"), ExpectedLocation))
		{
			OutError = TEXT("Could not resolve actor or expected location for location assertion.");
			return false;
		}

		const float Tolerance = GetNumberField(Step.Data, TEXT("tolerance"), 10.0f);
		const float Distance = FVector::Distance(Actor->GetActorLocation(), ExpectedLocation);
		if (Distance > Tolerance)
		{
			OutError = FString::Printf(TEXT("Location assertion failed. Distance %.2f exceeded tolerance %.2f."), Distance, Tolerance);
			return false;
		}

		return true;
	}

	if (Step.Type.Equals(TEXT("assertWidgetVisible"), ESearchCase::IgnoreCase))
	{
		UWidget* Widget = FindWidget(Step.Data);
		if (Widget == nullptr)
		{
			OutError = TEXT("Expected widget was not found.");
			return false;
		}

		if (!IsWidgetVisible(Widget))
		{
			OutError = FString::Printf(TEXT("Widget '%s' was not visible."), *Widget->GetName());
			return false;
		}

		return true;
	}

	if (Step.Type.Equals(TEXT("assertWidgetText"), ESearchCase::IgnoreCase))
	{
		UWidget* Widget = FindWidget(Step.Data);
		UTextBlock* TextBlock = Cast<UTextBlock>(Widget);
		FString ExpectedText;
		if (TextBlock == nullptr || !Step.Data->TryGetStringField(TEXT("text"), ExpectedText))
		{
			OutError = TEXT("Widget text assertion requires a TextBlock widget and text field.");
			return false;
		}

		const FString ActualText = TextBlock->GetText().ToString();
		if (!ActualText.Equals(ExpectedText))
		{
			OutError = FString::Printf(TEXT("Widget text assertion failed. Expected '%s', got '%s'."), *ExpectedText, *ActualText);
			return false;
		}

		return true;
	}

	if (Step.Type.Equals(TEXT("clickWidget"), ESearchCase::IgnoreCase))
	{
		return ClickWidget(Step.Data, OutError);
	}

	OutError = FString::Printf(TEXT("Unknown step type: %s"), *Step.Type);
	return false;
}

bool FTestPlayRunner::TickWaitStep(const FTestPlayStep& Step, float DeltaSeconds, FString& OutError)
{
	const float Duration = GetNumberField(Step.Data, TEXT("seconds"), GetNumberField(Step.Data, TEXT("wait"), 1.0f));
	return StepElapsedSeconds >= Duration;
}

bool FTestPlayRunner::TickWaitForActorStep(const FTestPlayStep& Step, float DeltaSeconds, FString& OutError)
{
	if (FindActor(Step.Data) != nullptr)
	{
		return true;
	}

	const float Timeout = GetNumberField(Step.Data, TEXT("timeout"), 5.0f);
	if (StepElapsedSeconds >= Timeout)
	{
		OutError = FString::Printf(TEXT("Timed out waiting for actor after %.2f seconds."), Timeout);
	}

	return false;
}

bool FTestPlayRunner::TickWaitForWidgetStep(const FTestPlayStep& Step, float DeltaSeconds, FString& OutError)
{
	UWidget* Widget = FindWidget(Step.Data);
	if (Widget != nullptr)
	{
		bool bRequireVisible = false;
		if (!Step.Data->TryGetBoolField(TEXT("visible"), bRequireVisible) || !bRequireVisible || IsWidgetVisible(Widget))
		{
			return true;
		}
	}

	const float Timeout = GetNumberField(Step.Data, TEXT("timeout"), 5.0f);
	if (StepElapsedSeconds >= Timeout)
	{
		OutError = FString::Printf(TEXT("Timed out waiting for widget after %.2f seconds."), Timeout);
	}

	return false;
}

bool FTestPlayRunner::TickInputActionStep(const FTestPlayStep& Step, float DeltaSeconds, FString& OutError)
{
	const float Duration = GetNumberField(Step.Data, TEXT("duration"), 0.1f);
	const bool bRelease = StepElapsedSeconds >= Duration;

	if (!InjectInputAction(Step.Data, bRelease, OutError))
	{
		return false;
	}

	return bRelease;
}

AActor* FTestPlayRunner::FindActor(const TSharedPtr<FJsonObject>& Data) const
{
	if (!World.IsValid() || !Data.IsValid())
	{
		return nullptr;
	}

	FString Tag;
	if (Data->TryGetStringField(TEXT("actorTag"), Tag) || Data->TryGetStringField(TEXT("tag"), Tag))
	{
		return FindActorByTag(FName(*Tag));
	}

	FString Name;
	if (Data->TryGetStringField(TEXT("actorName"), Name) || Data->TryGetStringField(TEXT("name"), Name))
	{
		return FindActorByName(Name);
	}

	return nullptr;
}

AActor* FTestPlayRunner::FindActorByTag(const FName& Tag) const
{
	if (!World.IsValid())
	{
		return nullptr;
	}

	for (TActorIterator<AActor> ActorIt(World.Get()); ActorIt; ++ActorIt)
	{
		AActor* Actor = *ActorIt;
		if (Actor && Actor->ActorHasTag(Tag))
		{
			return Actor;
		}
	}

	return nullptr;
}

AActor* FTestPlayRunner::FindActorByName(const FString& Name) const
{
	if (!World.IsValid())
	{
		return nullptr;
	}

	for (TActorIterator<AActor> ActorIt(World.Get()); ActorIt; ++ActorIt)
	{
		AActor* Actor = *ActorIt;
		if (Actor && Actor->GetName().Equals(Name))
		{
			return Actor;
		}
	}

	return nullptr;
}

UWidget* FTestPlayRunner::FindWidget(const TSharedPtr<FJsonObject>& Data) const
{
	if (!World.IsValid() || !Data.IsValid())
	{
		return nullptr;
	}

	FString WidgetName;
	if (!Data->TryGetStringField(TEXT("name"), WidgetName) && !Data->TryGetStringField(TEXT("widget"), WidgetName))
	{
		return nullptr;
	}

	for (TObjectIterator<UUserWidget> WidgetIt; WidgetIt; ++WidgetIt)
	{
		UUserWidget* UserWidget = *WidgetIt;
		if (!UserWidget || UserWidget->GetWorld() != World.Get())
		{
			continue;
		}

		if (UserWidget->GetName().Equals(WidgetName))
		{
			return UserWidget;
		}

		if (UserWidget->WidgetTree)
		{
			if (UWidget* FoundWidget = UserWidget->WidgetTree->FindWidget(FName(*WidgetName)))
			{
				return FoundWidget;
			}
		}
	}

	return nullptr;
}

bool FTestPlayRunner::ClickWidget(const TSharedPtr<FJsonObject>& Data, FString& OutError)
{
	if (!FSlateApplication::IsInitialized())
	{
		OutError = TEXT("Slate application is not initialized.");
		return false;
	}

	UWidget* Widget = FindWidget(Data);
	if (Widget == nullptr)
	{
		OutError = TEXT("Expected widget was not found for click.");
		return false;
	}

	if (!IsWidgetVisible(Widget))
	{
		OutError = FString::Printf(TEXT("Widget '%s' is not visible for click."), *Widget->GetName());
		return false;
	}

	TSharedPtr<SWidget> CachedWidget = Widget->GetCachedWidget();
	if (!CachedWidget.IsValid())
	{
		OutError = FString::Printf(TEXT("Widget '%s' has no cached Slate widget for click."), *Widget->GetName());
		return false;
	}

	const FGeometry& Geometry = Widget->GetCachedGeometry();
	const FVector2D LocalSize = FVector2D(Geometry.GetLocalSize());
	if (LocalSize.X <= 0.0 || LocalSize.Y <= 0.0)
	{
		OutError = FString::Printf(TEXT("Widget '%s' has no valid geometry for click."), *Widget->GetName());
		return false;
	}

	FVector2D Offset(0.5, 0.5);
	ReadVector2D(Data, TEXT("offset"), Offset);
	const FVector2D LocalPosition(LocalSize.X * Offset.X, LocalSize.Y * Offset.Y);
	const FVector2D ScreenPosition = FVector2D(Geometry.LocalToAbsolute(LocalPosition));

	FKey MouseButton = EKeys::LeftMouseButton;
	FString ButtonName;
	if (Data.IsValid() && Data->TryGetStringField(TEXT("button"), ButtonName))
	{
		if (ButtonName.Equals(TEXT("RightMouseButton"), ESearchCase::IgnoreCase) || ButtonName.Equals(TEXT("Right"), ESearchCase::IgnoreCase))
		{
			MouseButton = EKeys::RightMouseButton;
		}
		else if (ButtonName.Equals(TEXT("MiddleMouseButton"), ESearchCase::IgnoreCase) || ButtonName.Equals(TEXT("Middle"), ESearchCase::IgnoreCase))
		{
			MouseButton = EKeys::MiddleMouseButton;
		}
	}

	FModifierKeysState ModifierKeys;
	TSet<FKey> PressedButtons;
	const FPointerEvent MoveEvent(0, ScreenPosition, ScreenPosition, PressedButtons, EKeys::Invalid, 0.0f, ModifierKeys);
	FSlateApplication::Get().ProcessMouseMoveEvent(MoveEvent, true);

	PressedButtons.Add(MouseButton);
	const FPointerEvent DownEvent(0, ScreenPosition, ScreenPosition, PressedButtons, MouseButton, 0.0f, ModifierKeys);
	FSlateApplication::Get().ProcessMouseButtonDownEvent(TSharedPtr<FGenericWindow>(), DownEvent);

	PressedButtons.Empty();
	const FPointerEvent UpEvent(0, ScreenPosition, ScreenPosition, PressedButtons, MouseButton, 0.0f, ModifierKeys);
	FSlateApplication::Get().ProcessMouseButtonUpEvent(UpEvent);

	Result.AddLog(FString::Printf(TEXT("Clicked widget '%s' at %.1f, %.1f."), *Widget->GetName(), ScreenPosition.X, ScreenPosition.Y));
	return true;
}

bool FTestPlayRunner::IsWidgetVisible(const UWidget* Widget) const
{
	if (Widget == nullptr)
	{
		return false;
	}

	const ESlateVisibility Visibility = Widget->GetVisibility();
	return Visibility != ESlateVisibility::Collapsed && Visibility != ESlateVisibility::Hidden;
}

bool FTestPlayRunner::InjectInputAction(const TSharedPtr<FJsonObject>& Data, bool bRelease, FString& OutError)
{
	if (!World.IsValid() || !Data.IsValid())
	{
		OutError = TEXT("Input action step has no valid PIE world or data.");
		return false;
	}

	FString ActionPath;
	if (!Data->TryGetStringField(TEXT("action"), ActionPath))
	{
		OutError = TEXT("Input action step requires an action asset path.");
		return false;
	}

	UInputAction* Action = LoadObject<UInputAction>(nullptr, *ActionPath);
	if (Action == nullptr)
	{
		OutError = FString::Printf(TEXT("Could not load input action: %s"), *ActionPath);
		return false;
	}

	const int32 PlayerIndex = static_cast<int32>(GetNumberField(Data, TEXT("player"), 0.0f));
	APlayerController* PlayerController = nullptr;
	int32 CurrentPlayerIndex = 0;
	for (FConstPlayerControllerIterator PlayerIt = World->GetPlayerControllerIterator(); PlayerIt; ++PlayerIt)
	{
		if (CurrentPlayerIndex == PlayerIndex)
		{
			PlayerController = PlayerIt->Get();
			break;
		}

		++CurrentPlayerIndex;
	}

	if (PlayerController == nullptr)
	{
		OutError = FString::Printf(TEXT("Could not find player controller %d."), PlayerIndex);
		return false;
	}

	UEnhancedPlayerInput* EnhancedPlayerInput = Cast<UEnhancedPlayerInput>(PlayerController->PlayerInput);
	if (EnhancedPlayerInput == nullptr)
	{
		OutError = TEXT("Player controller does not use UEnhancedPlayerInput.");
		return false;
	}

	FInputActionValue ActionValue;
	if (!ParseActionValue(Action, Data, bRelease, ActionValue))
	{
		OutError = FString::Printf(TEXT("Could not parse value for input action: %s"), *ActionPath);
		return false;
	}

	EnhancedPlayerInput->InjectInputForAction(Action, ActionValue);
	return true;
}

bool FTestPlayRunner::ParseActionValue(UInputAction* Action, const TSharedPtr<FJsonObject>& Data, bool bRelease, FInputActionValue& OutValue) const
{
	if (Action == nullptr)
	{
		return false;
	}

	if (bRelease)
	{
		OutValue = FInputActionValue(Action->ValueType, FVector::ZeroVector);
		return true;
	}

	if (Action->ValueType == EInputActionValueType::Boolean)
	{
		bool bValue = true;
		Data->TryGetBoolField(TEXT("value"), bValue);
		OutValue = FInputActionValue(bValue);
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Data->TryGetArrayField(TEXT("value"), Values) || Values == nullptr)
	{
		OutValue = FInputActionValue(Action->ValueType, FVector(1.0f, 0.0f, 0.0f));
		return true;
	}

	FVector VectorValue = FVector::ZeroVector;
	if (Values->Num() > 0)
	{
		VectorValue.X = static_cast<float>((*Values)[0]->AsNumber());
	}
	if (Values->Num() > 1)
	{
		VectorValue.Y = static_cast<float>((*Values)[1]->AsNumber());
	}
	if (Values->Num() > 2)
	{
		VectorValue.Z = static_cast<float>((*Values)[2]->AsNumber());
	}

	OutValue = FInputActionValue(Action->ValueType, VectorValue);
	return true;
}

bool FTestPlayRunner::ReadVector2D(const TSharedPtr<FJsonObject>& Data, const FString& FieldName, FVector2D& OutVector) const
{
	if (!Data.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Data->TryGetArrayField(FieldName, Values) || Values == nullptr || Values->Num() < 2)
	{
		return false;
	}

	OutVector = FVector2D(
		(*Values)[0]->AsNumber(),
		(*Values)[1]->AsNumber());
	return true;
}

bool FTestPlayRunner::ReadVector(const TSharedPtr<FJsonObject>& Data, const FString& FieldName, FVector& OutVector) const
{
	if (!Data.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Data->TryGetArrayField(FieldName, Values) || Values == nullptr || Values->Num() < 3)
	{
		return false;
	}

	OutVector = FVector(
		static_cast<float>((*Values)[0]->AsNumber()),
		static_cast<float>((*Values)[1]->AsNumber()),
		static_cast<float>((*Values)[2]->AsNumber()));
	return true;
}

FString FTestPlayRunner::GetStepLabel(const FTestPlayStep& Step) const
{
	return FString::Printf(TEXT("%s"), *Step.Type);
}

float FTestPlayRunner::GetNumberField(const TSharedPtr<FJsonObject>& Data, const FString& FieldName, float DefaultValue) const
{
	if (!Data.IsValid())
	{
		return DefaultValue;
	}

	double Value = DefaultValue;
	return Data->TryGetNumberField(FieldName, Value) ? static_cast<float>(Value) : DefaultValue;
}

void FTestPlayRunner::FailCurrentStep(const FString& Error)
{
	Result.FailedStep = CurrentStepIndex;
	Result.Error = Error;
	Result.AddLog(FString::Printf(TEXT("Step %d failed: %s"), CurrentStepIndex, *Error));
	UE_LOG(LogTestPlay, Error, TEXT("TESTPLAY_RESULT: FAILED - %s"), *Error);
	CompleteRunner(false);
}

void FTestPlayRunner::CompleteRunner(bool bSuccess)
{
	if (bComplete)
	{
		return;
	}

	bComplete = true;
	Result.bSuccess = bSuccess;
	Result.DurationSeconds = ElapsedSeconds;
	if (bSuccess)
	{
		Result.AddLog(TEXT("Spec completed successfully."));
		UE_LOG(LogTestPlay, Display, TEXT("TESTPLAY_RESULT: PASSED"));
	}
}
