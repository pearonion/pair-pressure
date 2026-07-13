#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "PairPressure/PPGameplayTypes.h"
#include "PPGameplayInterfaces.generated.h"

UINTERFACE(BlueprintType)
class VNHSIMULATOR_API UPPPhysicalStateInterface : public UInterface
{
	GENERATED_BODY()
};

class VNHSIMULATOR_API IPPPhysicalStateInterface
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Pair Pressure|Physical State")
	EPPPhysicalState GetPhysicalState() const;

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Pair Pressure|Physical State")
	float GetDazeNormalized() const;

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Pair Pressure|Physical State")
	void RequestRagdoll(float DazeAmount);

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Pair Pressure|Physical State")
	void RequestRecovery();
};

UINTERFACE(BlueprintType)
class VNHSIMULATOR_API UPPImpactReceiver : public UInterface
{
	GENERATED_BODY()
};

class VNHSIMULATOR_API IPPImpactReceiver
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Pair Pressure|Impact")
	void ReceiveImpactData(const FPPImpactData& ImpactData);
};

UINTERFACE(BlueprintType)
class VNHSIMULATOR_API UPPTeamMember : public UInterface
{
	GENERATED_BODY()
};

class VNHSIMULATOR_API IPPTeamMember
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Pair Pressure|Team")
	int32 GetTeamId() const;

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Pair Pressure|Team")
	AActor* GetPartner() const;

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Pair Pressure|Team")
	bool IsFriendlyTo(const AActor* OtherActor) const;
};

UINTERFACE(BlueprintType)
class VNHSIMULATOR_API UPPCarryable : public UInterface
{
	GENERATED_BODY()
};

class VNHSIMULATOR_API IPPCarryable
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Pair Pressure|Carry")
	bool CanCarry(AActor* RequestedCarrier) const;

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Pair Pressure|Carry")
	void OnCarryStarted(AActor* NewCarrier);

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Pair Pressure|Carry")
	void OnCarryEnded(AActor* PreviousCarrier);
};

UINTERFACE(BlueprintType)
class VNHSIMULATOR_API UPPUIDataProvider : public UInterface
{
	GENERATED_BODY()
};

class VNHSIMULATOR_API IPPUIDataProvider
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Pair Pressure|UI")
	FPPHUDSnapshot GetHUDSnapshot() const;
};

UINTERFACE(BlueprintType)
class VNHSIMULATOR_API UPPInteractionPrompt : public UInterface
{
	GENERATED_BODY()
};

class VNHSIMULATOR_API IPPInteractionPrompt
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Pair Pressure|Interaction")
	FText GetPromptText() const;

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Pair Pressure|Interaction")
	bool IsPromptAvailable(AActor* RequestingActor) const;
};
