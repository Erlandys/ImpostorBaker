﻿// Fill out your copyright notice in the Description page of Project Settings.

#include "ImpostorComponentsManager.h"
#include "ImpostorData.h"
#include "ImpostorBakerUtilities.h"
#include "ImpostorLightingManager.h"

void UImpostorComponentsManager::Initialize()
{
	ReferencedMeshComponent = NewObject<UStaticMeshComponent>(GetTransientPackage());
	AddComponent(ReferencedMeshComponent);

	MeshStandComponent = NewObject<UStaticMeshComponent>(GetTransientPackage());
	AddComponent(MeshStandComponent);
	MeshStandComponent->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/EditorMeshes/EditorCube.EditorCube")));

	ImpostorStandComponent = NewObject<UStaticMeshComponent>(GetTransientPackage());
	AddComponent(ImpostorStandComponent);
	ImpostorStandComponent->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/EditorMeshes/EditorCube.EditorCube")));
}

void UImpostorComponentsManager::Update()
{
	UpdateComponentsData();
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

void UImpostorComponentsManager::UpdateComponentsData()
{
	ReferencedMeshComponent->SetStaticMesh(ImpostorData->ReferencedMesh);
	ReferencedMeshComponent->SetForcedLodModel(0);
	ReferencedMeshComponent->SetRelativeLocation(FVector(ReferencedMeshComponent->Bounds.SphereRadius * -2.25f, 0.f, 10.f));

	ObjectRadius = ReferencedMeshComponent->Bounds.SphereRadius;
	OffsetVector = ReferencedMeshComponent->Bounds.Origin - ReferencedMeshComponent->GetComponentLocation();
	DebugTexelSize = ObjectRadius / 16.f;

	const FVector Scale(ObjectRadius / 256.f * 2.f, ObjectRadius / 256.f * 2.f, 0.05f);
	{
		const FVector RelativeOffset(ObjectRadius * 2.25f, 0.f, -10.f);
		ImpostorStandComponent->SetWorldScale3D(Scale);
		ImpostorStandComponent->SetRelativeLocation(RelativeOffset);
	}

	{
		const FVector RelativeOffset(ObjectRadius * -2.25f, 0.f, -10.f);
		MeshStandComponent->SetWorldScale3D(Scale);
		MeshStandComponent->SetRelativeLocation(RelativeOffset);
	}

	if (ImpostorData->ImpostorType == EImpostorLayoutType::TraditionalBillboards)
	{
		SetupTraditionalBillboardLayout();
	}
	else
	{
		SetupOctahedronLayout();
	}

	SetupPreviewMeshes();
}

void UImpostorComponentsManager::SetupOctahedronLayout()
{
	NumHorizontalFrames = NumVerticalFrames = ImpostorData->FramesCount;

	ViewCaptureVectors = {};
	ViewCaptureVectors.Reserve(ImpostorData->FramesCount * ImpostorData->FramesCount);
	ViewCaptureVectors.SetNumUninitialized(ImpostorData->FramesCount * ImpostorData->FramesCount);

	for (int32 Y = 0; Y < ImpostorData->FramesCount; Y++)
	{
		for (int32 X = 0; X < ImpostorData->FramesCount; X++)
		{
			ViewCaptureVectors[Y * ImpostorData->FramesCount + X] = FImpostorBakerUtilities::GetGridVector(X, Y, ImpostorData->FramesCount, ImpostorData->ImpostorType);
		}
	}
}

void UImpostorComponentsManager::SetupTraditionalBillboardLayout()
{
	int32 HorizontalSlices;
	if (ImpostorData->bGenerateTwoSidedGeometry)
	{
		HorizontalSlices = FMath::Min(2, ImpostorData->HorizontalFramesCount) * 2 + FMath::Max(0, ImpostorData->HorizontalFramesCount - 2) * 4;
	}
	else
	{
		HorizontalSlices = FMath::Min(2, ImpostorData->HorizontalFramesCount) + FMath::Max(0, ImpostorData->HorizontalFramesCount - 2) * 2;
	}

	NumHorizontalFrames = HorizontalSlices + (ImpostorData->bCaptureTopFrame ? 1 : 0);
	NumVerticalFrames = 1;

	ViewCaptureVectors = {};
	ViewCaptureVectors.Reserve(NumHorizontalFrames * NumVerticalFrames);
	ViewCaptureVectors.SetNumUninitialized(NumHorizontalFrames * NumVerticalFrames);

	const float SliceRadius = FMath::DegreesToRadians((ImpostorData->bGenerateTwoSidedGeometry ? 360.f : 180.f) / (ImpostorData->bGenerateTwoSidedGeometry ? HorizontalSlices : HorizontalSlices));
	for (int32 Index = 0; Index < HorizontalSlices; Index++)
	{
		ViewCaptureVectors[Index] = FVector(FMath::Cos(SliceRadius * Index), FMath::Sin(SliceRadius * Index), 0.f);
	}

	if (ImpostorData->bCaptureTopFrame)
	{
		BillboardTopFrame = HorizontalSlices;
		ViewCaptureVectors[HorizontalSlices] = FVector::UpVector;
	}
	else
	{
		BillboardTopFrame = -1;
	}
}

void UImpostorComponentsManager::SetupPreviewMeshes()
{
	for (int32 Index = 0; Index < ViewCaptureVectors.Num(); Index++)
	{
		if (ImpostorData->bPreviewCaptureSphere)
		{
			const FVector& CaptureVector = ViewCaptureVectors[Index];

			FTransform Transform = FTransform::Identity;
			Transform.SetLocation(CaptureVector * (ObjectRadius * 1.1f * 1.1f) + OffsetVector + FVector(ReferencedMeshComponent->Bounds.SphereRadius * -2.25f, 0.f, 0.f));
			Transform.SetRotation(FRotationMatrix::MakeFromYZ(CaptureVector.GetSafeNormal(), FVector::UpVector).ToQuat());

			if (!VisualizedMeshes.IsValidIndex(Index))
			{
				UStaticMeshComponent* NewComponent = NewObject<UStaticMeshComponent>(GetTransientPackage());
				AddComponent(NewComponent);

				VisualizedMeshes.Add(NewComponent);
				NewComponent->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/EditorMeshes/Camera/SM_CineCam.SM_CineCam")));
			}

			UStaticMeshComponent* CaptureVisualization = VisualizedMeshes[Index];
			CaptureVisualization->SetRelativeTransform(Transform);
		}
	}
	if (!ImpostorData->bPreviewCaptureSphere)
	{
		for (int32 Index = 0; Index < VisualizedMeshes.Num(); Index++)
		{
			DestroyComponent(VisualizedMeshes[Index]);
		}
		VisualizedMeshes = {};
	}
	else if (VisualizedMeshes.Num() > NumHorizontalFrames * NumVerticalFrames)
	{
		for (int32 Index = VisualizedMeshes.Num() - 1; Index >= NumHorizontalFrames * NumVerticalFrames; Index--)
		{
			DestroyComponent(VisualizedMeshes[Index]);
			VisualizedMeshes.RemoveAt(Index);
		}
	}

	SetOverlayText("FramesCount", "Frames Count", LexToString(NumHorizontalFrames * NumVerticalFrames));

	const int32 FrameSize = ImpostorData->ImpostorType == EImpostorLayoutType::TraditionalBillboards ? ImpostorData->FrameSize : (ImpostorData->Resolution / ImpostorData->FramesCount);
	SetOverlayText("FrameSize", "Single Frame Size", LexToString(FrameSize) + "x" + LexToString(FrameSize));

	const int32 TextureSizeX = ImpostorData->ImpostorType == EImpostorLayoutType::TraditionalBillboards ? NumHorizontalFrames * ImpostorData->FrameSize : ImpostorData->Resolution;
	const int32 TextureSizeY = ImpostorData->ImpostorType == EImpostorLayoutType::TraditionalBillboards ? NumVerticalFrames * ImpostorData->FrameSize : ImpostorData->Resolution;
	SetOverlayText("TextureSize", "Texture Size", LexToString(TextureSizeX) + "x" + LexToString(TextureSizeY));
}

FVector2D UImpostorComponentsManager::GetRenderTargetSize() const
{
	switch (ImpostorData->ImpostorType)
	{
	default: check(false);
	case EImpostorLayoutType::FullSphereView:
	case EImpostorLayoutType::UpperHemisphereOnly: return FVector2D(ImpostorData->Resolution, ImpostorData->Resolution);
	case EImpostorLayoutType::TraditionalBillboards: return FVector2D(ImpostorData->FrameSize * NumHorizontalFrames, ImpostorData->FrameSize * NumVerticalFrames);
	}
}