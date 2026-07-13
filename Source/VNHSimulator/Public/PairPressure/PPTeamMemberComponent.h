#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PairPressure/PPGameplayInterfaces.h"
#include "PPTeamMemberComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPPPartnerChanged, int32, NewTeamId, AActor*, NewPartner);

UCLASS(ClassGroup = (PairPressure), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class VNHSIMULATOR_API UPPTeamMemberComponent : public UActorComponent, public IPPTeamMember
{
	GENERATED_BODY()

public:
	UPPTeamMemberComponent();

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual int32 GetTeamId_Implementation() const override { return TeamId; }
	virtual AActor* GetPartner_Implementation() const override { return Partner; }
	virtual bool IsFriendlyTo_Implementation(const AActor* OtherActor) const override;

	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Pair Pressure|Team")
	void AssignTeam(int32 NewTeamId, AActor* NewPartner);

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Team")
	int32 GetAssignedTeamId() const { return TeamId; }

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Team")
	AActor* GetAssignedPartner() const { return Partner; }

	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Pair Pressure|Team")
	void SetFinishState(bool bNewIsHome, bool bNewTeamFinished);

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Team")
	bool IsHome() const { return bIsHome; }

	UFUNCTION(BlueprintPure, Category = "Pair Pressure|Team")
	bool IsTeamFinished() const { return bTeamFinished; }

	UPROPERTY(BlueprintAssignable, Category = "Pair Pressure|Team")
	FPPPartnerChanged OnPartnerChanged;

	static UPPTeamMemberComponent* FindTeamComponent(const AActor* Actor);

private:
	UFUNCTION()
	void RefreshAutomaticAssignment();

	UFUNCTION()
	void OnRep_TeamAssignment();

	UPROPERTY(ReplicatedUsing = OnRep_TeamAssignment, VisibleInstanceOnly, Category = "Pair Pressure|Team")
	int32 TeamId = INDEX_NONE;

	UPROPERTY(ReplicatedUsing = OnRep_TeamAssignment, VisibleInstanceOnly, Category = "Pair Pressure|Team")
	TObjectPtr<AActor> Partner = nullptr;

	UPROPERTY(ReplicatedUsing = OnRep_TeamAssignment, VisibleInstanceOnly, Category = "Pair Pressure|Team")
	bool bIsHome = false;

	UPROPERTY(ReplicatedUsing = OnRep_TeamAssignment, VisibleInstanceOnly, Category = "Pair Pressure|Team")
	bool bTeamFinished = false;

	FTimerHandle AssignmentTimerHandle;
};
