#pragma once

#include "CoreMinimal.h"
#include "TestPlaySpec.h"

class AActor;
class UInputAction;
class UWidget;
class UWorld;

class TESTPLAY_API FTestPlayRunner
{
public:
	FTestPlayRunner(UWorld* InWorld, const FTestPlaySpec& InSpec);

	void Start();
	void Tick(float DeltaSeconds);

	bool IsComplete() const { return bComplete; }
	bool Succeeded() const { return Result.bSuccess; }
	const FTestPlayResult& GetResult() const { return Result; }

private:
	bool TickCurrentStep(float DeltaSeconds);
	bool ExecuteInstantStep(const FTestPlayStep& Step, FString& OutError);
	bool TickWaitStep(const FTestPlayStep& Step, float DeltaSeconds, FString& OutError);
	bool TickWaitForActorStep(const FTestPlayStep& Step, float DeltaSeconds, FString& OutError);
	bool TickWaitForWidgetStep(const FTestPlayStep& Step, float DeltaSeconds, FString& OutError);
	bool TickInputActionStep(const FTestPlayStep& Step, float DeltaSeconds, FString& OutError);

	AActor* FindActor(const TSharedPtr<FJsonObject>& Data) const;
	AActor* FindActorByTag(const FName& Tag) const;
	AActor* FindActorByName(const FString& Name) const;
	UWidget* FindWidget(const TSharedPtr<FJsonObject>& Data) const;

	bool InjectInputAction(const TSharedPtr<FJsonObject>& Data, bool bRelease, FString& OutError);
	bool ClickWidget(const TSharedPtr<FJsonObject>& Data, FString& OutError);
	bool IsWidgetVisible(const UWidget* Widget) const;
	bool ParseActionValue(UInputAction* Action, const TSharedPtr<FJsonObject>& Data, bool bRelease, struct FInputActionValue& OutValue) const;
	bool ReadVector2D(const TSharedPtr<FJsonObject>& Data, const FString& FieldName, FVector2D& OutVector) const;
	bool ReadVector(const TSharedPtr<FJsonObject>& Data, const FString& FieldName, FVector& OutVector) const;
	FString GetStepLabel(const FTestPlayStep& Step) const;
	float GetNumberField(const TSharedPtr<FJsonObject>& Data, const FString& FieldName, float DefaultValue) const;
	void FailCurrentStep(const FString& Error);
	void CompleteRunner(bool bSuccess);

	TWeakObjectPtr<UWorld> World;
	FTestPlaySpec Spec;
	FTestPlayResult Result;
	int32 CurrentStepIndex = 0;
	float ElapsedSeconds = 0.0f;
	float StepElapsedSeconds = 0.0f;
	bool bStepStarted = false;
	bool bComplete = false;
};
