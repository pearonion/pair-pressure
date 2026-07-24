#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "PairPressure/Interfaces/PPMascotSelectionInterface.h"
#include "VNHGameplayTypes.h"
#include "VNHPlayerState.generated.h"

UCLASS()
class VNHSIMULATOR_API AVNHPlayerState : public APlayerState, public IPPMascotSelectionInterface
{
	GENERATED_BODY()

public:
	AVNHPlayerState();

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void CopyProperties(APlayerState* TargetPlayerStateBase) override;

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	EVNHPlayerRole GetRole() const { return AssignedRole; }

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	bool IsAlien() const { return AssignedRole == EVNHPlayerRole::Alien; }

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	bool IsHuman() const { return AssignedRole == EVNHPlayerRole::Human; }

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	bool IsHunter() const { return AssignedRole == EVNHPlayerRole::Hunter; }

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	FText GetRoleDisplayText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	FText GetPrivateRoleRevealText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	FText GetRoleGoalText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	FText GetLightErrandText() const { return LightErrandText; }

	UFUNCTION(BlueprintPure, Category = "VNH|Player")
	bool IsPreRoundReady() const { return bPreRoundReady; }

	UFUNCTION(BlueprintPure, Category = "VNH|Lobby")
	int32 GetLobbyTeamId() const { return LobbyTeamId; }

	void SetRole(EVNHPlayerRole NewRole);
	void SetLightErrandText(const FText& NewLightErrandText);
	void SetPreRoundReady(bool bNewPreRoundReady);
	void SetLobbyTeamId(int32 NewLobbyTeamId);

	virtual FName GetSelectedMascotRowName_Implementation() const override;
	virtual void ApplySelectedMascotRowName_Implementation(FName InMascotRowName) override;

private:
	UPROPERTY(ReplicatedUsing = OnRep_AssignedRole, BlueprintReadOnly, Category = "VNH|Player", meta = (AllowPrivateAccess = "true"))
	EVNHPlayerRole AssignedRole = EVNHPlayerRole::Unassigned;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Player", meta = (AllowPrivateAccess = "true"))
	FText LightErrandText;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Player", meta = (AllowPrivateAccess = "true"))
	bool bPreRoundReady = false;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Lobby", meta = (AllowPrivateAccess = "true"))
	int32 LobbyTeamId = INDEX_NONE;

	UPROPERTY(ReplicatedUsing = OnRep_SelectedMascotRowName, BlueprintReadOnly, Category = "Pair Pressure|Mascot", meta = (AllowPrivateAccess = "true"))
	FName SelectedMascotRowName = NAME_None;

	UFUNCTION()
	void OnRep_AssignedRole();

	UFUNCTION()
	void OnRep_SelectedMascotRowName();
};
