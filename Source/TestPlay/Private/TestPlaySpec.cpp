#include "TestPlaySpec.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TaskEvidenceBuilder.h"

namespace TestPlaySpec
{
	bool TryReadStep(const TSharedPtr<FJsonObject>& StepObject, FTestPlayStep& OutStep, FString& OutError)
	{
		if (!StepObject.IsValid())
		{
			OutError = TEXT("Step entry is not an object.");
			return false;
		}

		if (StepObject->HasTypedField<EJson::String>(TEXT("type")))
		{
			OutStep.Type = StepObject->GetStringField(TEXT("type"));
			OutStep.Data = StepObject;
			return true;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : StepObject->Values)
		{
			OutStep.Type = Pair.Key;
			if (Pair.Value->Type == EJson::Object)
			{
				OutStep.Data = Pair.Value->AsObject();
			}
			else
			{
				OutStep.Data = MakeShared<FJsonObject>();
				OutStep.Data->SetField(Pair.Key, Pair.Value);
			}
			return true;
		}

		OutError = TEXT("Step object has no fields.");
		return false;
	}
}

bool FTestPlaySpec::LoadFromFile(const FString& FilePath, FTestPlaySpec& OutSpec, FString& OutError)
{
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
	{
		OutError = FString::Printf(TEXT("Could not read spec file: %s"), *FilePath);
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		OutError = FString::Printf(TEXT("Could not parse JSON spec: %s"), *FilePath);
		return false;
	}

	FTestPlaySpec LoadedSpec;
	LoadedSpec.SourceFile = FilePath;
	RootObject->TryGetStringField(TEXT("name"), LoadedSpec.Name);
	RootObject->TryGetStringField(TEXT("map"), LoadedSpec.Map);

	double Timeout = LoadedSpec.Timeout;
	if (RootObject->TryGetNumberField(TEXT("timeout"), Timeout))
	{
		LoadedSpec.Timeout = static_cast<float>(Timeout);
	}

	const TArray<TSharedPtr<FJsonValue>>* StepValues = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("steps"), StepValues) || StepValues == nullptr)
	{
		OutError = TEXT("Spec must contain a steps array.");
		return false;
	}

	for (int32 StepIndex = 0; StepIndex < StepValues->Num(); ++StepIndex)
	{
		FTestPlayStep Step;
		if (!TestPlaySpec::TryReadStep((*StepValues)[StepIndex]->AsObject(), Step, OutError))
		{
			OutError = FString::Printf(TEXT("Invalid step %d: %s"), StepIndex, *OutError);
			return false;
		}

		LoadedSpec.Steps.Add(Step);
	}

	if (LoadedSpec.Name.IsEmpty())
	{
		LoadedSpec.Name = FPaths::GetBaseFilename(FilePath);
	}

	OutSpec = MoveTemp(LoadedSpec);
	return true;
}

void FTestPlayResult::AddLog(const FString& Message)
{
	LogLines.Add(Message);
}

bool FTestPlayResult::WriteToFile(const FString& FilePath, FString& OutError) const
{
	TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetStringField(TEXT("suite"), SuiteName);
	RootObject->SetBoolField(TEXT("success"), bSuccess);
	RootObject->SetNumberField(TEXT("durationSeconds"), DurationSeconds);
	RootObject->SetNumberField(TEXT("failedStep"), FailedStep);
	RootObject->SetStringField(TEXT("error"), Error);

	TArray<TSharedPtr<FJsonValue>> LogValues;
	for (const FString& Line : LogLines)
	{
		LogValues.Add(MakeShared<FJsonValueString>(Line));
	}
	RootObject->SetArrayField(TEXT("log"), LogValues);

	FString JsonText;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	if (!FJsonSerializer::Serialize(RootObject, Writer))
	{
		OutError = TEXT("Could not serialize result JSON.");
		return false;
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
	if (!FFileHelper::SaveStringToFile(JsonText, *FilePath))
	{
		OutError = FString::Printf(TEXT("Could not write result file: %s"), *FilePath);
		return false;
	}

	return true;
}

bool FTestPlayResult::WriteEvidenceToDefaultLocation(const FString& ResultFilePath, FString& OutEvidencePath, FString& OutError) const
{
	FTaskEvidenceBuilder Evidence(TEXT("TestPlay"), TEXT("RunSpec"));
	Evidence
		.SetStatus(bSuccess ? TEXT("passed") : TEXT("failed"))
		.SetSummary(bSuccess ? TEXT("TestPlay spec completed successfully.") : TEXT("TestPlay spec failed."), Error)
		.AddFact(TEXT("testplay.suite"), SuiteName)
		.AddFact(TEXT("testplay.success"), bSuccess)
		.AddFact(TEXT("testplay.duration_seconds"), static_cast<double>(DurationSeconds))
		.AddFact(TEXT("testplay.failed_step"), FailedStep)
		.AddArtifact(ResultFilePath, TEXT("test_result"), TEXT("application/json"), TEXT("Original TestPlay result JSON."));

	for (const FString& Line : LogLines)
	{
		Evidence.AddLog(TEXT("info"), TEXT("TestPlay"), Line);
	}

	return Evidence.WriteToDefaultLocation(OutEvidencePath, OutError);
}
