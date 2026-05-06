#pragma once

#include "CoreMinimal.h"

class FJsonObject;

struct TESTPLAY_API FTestPlayStep
{
	FString Type;
	TSharedPtr<FJsonObject> Data;
};

struct TESTPLAY_API FTestPlaySpec
{
	FString Name;
	FString Map;
	float Timeout = 60.0f;
	TArray<FTestPlayStep> Steps;
	FString SourceFile;

	static bool LoadFromFile(const FString& FilePath, FTestPlaySpec& OutSpec, FString& OutError);
};

struct TESTPLAY_API FTestPlayResult
{
	FString SuiteName;
	bool bSuccess = false;
	float DurationSeconds = 0.0f;
	int32 FailedStep = INDEX_NONE;
	FString Error;
	TArray<FString> LogLines;

	void AddLog(const FString& Message);
	bool WriteToFile(const FString& FilePath, FString& OutError) const;
};
