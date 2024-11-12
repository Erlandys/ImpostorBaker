// Fill out your copyright notice in the Description page of Project Settings.

#include "ImpostorRenderTargetsManager.h"

#include "ImpostorBakerSettings.h"
#include "ImpostorBakerUtilities.h"
#include "ImpostorMaterialsManager.h"
#include "ImpostorComponentsManager.h"
#include "ImpostorProceduralMeshManager.h"

#include "Engine/Canvas.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/SceneCaptureComponent2D.h"

void UImpostorRenderTargetsManager::Initialize()
{
	SceneCaptureComponent2D = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
	AddComponent(SceneCaptureComponent2D);
}

void UImpostorRenderTargetsManager::Update()
{
	AllocateRenderTargets();
	CreateRenderTargetMips();
	CreateAlphasScratchRenderTargets();
	FillMapsToSave();

	SceneCaptureSetup();

	if (ImpostorData->MapsToRender.Contains(EImpostorBakeMapType::BaseColor) &&
		ImpostorData->MapsToRender.Contains(EImpostorBakeMapType::CustomLighting) &&
		!ImpostorData->bCombineLightingAndColor)
	{
		SetOverlayText("CombineBaseColorLighting", "<TextBlock.ShadowedText>For</><TextBlock.ShadowedTextWarning> CustomLighting </><TextBlock.ShadowedText>to affect</><TextBlock.ShadowedTextWarning> BaseColor </><TextBlock.ShadowedText>it is necessary to enable</><TextBlock.ShadowedTextWarning> Combine Lighting and Color </><TextBlock.ShadowedText>parameter</>");
	}
	else
	{
		SetOverlayText("CombineBaseColorLighting", "");
	}
}

void UImpostorRenderTargetsManager::Tick()
{
	if (CurrentMap == EImpostorBakeMapType::None)
	{
		return;
	}

	if (FramesBeforeCapture-- > 0)
	{
		return;
	}

	CaptureImposterGrid();
	if (CurrentMap == EImpostorBakeMapType::BaseColor)
	{
		bCapturingFinalColor = true;
	}

	if (MapsToBake.Num() == 0)
	{
		FinalizeBaking();
		return;
	}

	PreparePostProcess(MapsToBake.Pop());
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

void UImpostorRenderTargetsManager::AllocateRenderTargets()
{
	TSet<EImpostorBakeMapType> UnusedMaps;
	TargetMaps.GetKeys(UnusedMaps);

	for (EImpostorBakeMapType MapType : ImpostorData->MapsToRender)
	{
		UnusedMaps.Remove(MapType);

		ETextureRenderTargetFormat Format;
		switch (MapType)
		{
		default: check(false);
		case EImpostorBakeMapType::BaseColor: Format = RTF_RGBA8_SRGB; break;
		case EImpostorBakeMapType::WorldNormal: Format = RTF_RGBA8; break;
		case EImpostorBakeMapType::Metallic:
		case EImpostorBakeMapType::Specular:
		case EImpostorBakeMapType::Roughness:
		case EImpostorBakeMapType::Opacity:
		case EImpostorBakeMapType::Subsurface:
		case EImpostorBakeMapType::Depth:
		case EImpostorBakeMapType::CustomLighting: Format = RTF_R8; break;
		}

		const FVector2D Size = GetManager<UImpostorComponentsManager>()->GetRenderTargetSize();
		if (UTextureRenderTarget2D* TargetMap = TargetMaps.FindRef(MapType))
		{
			if (TargetMap->RenderTargetFormat == Format)
			{
				if (TargetMap->SizeX != Size.X ||
					TargetMap->SizeY != Size.Y)
				{
					UKismetRenderingLibrary::ResizeRenderTarget2D(TargetMap, Size.X, Size.Y);
					continue;
				}
			}
		}

		UTextureRenderTarget2D* RenderTarget = UKismetRenderingLibrary::CreateRenderTarget2D(SceneWorld, Size.X, Size.Y, Format);
		check(RenderTarget);
		TargetMaps.Add(MapType, RenderTarget);
	}

	for (const EImpostorBakeMapType UnusedMap : UnusedMaps)
	{
		TargetMaps.Remove(UnusedMap);
	}
}

void UImpostorRenderTargetsManager::CreateRenderTargetMips()
{
	for (int32 MipIndex = 0; MipIndex < 8; MipIndex++)
	{
		const int32 MipSize = ImpostorData->SceneCaptureResolution / (1 << MipIndex);
		if (SceneCaptureMipChain.IsValidIndex(MipIndex))
		{
			if (UTextureRenderTarget2D* MipRenderTarget = SceneCaptureMipChain[MipIndex])
			{
				if (MipRenderTarget->SizeX == MipSize)
				{
					continue;
				}

				UKismetRenderingLibrary::ResizeRenderTarget2D(MipRenderTarget, MipSize, MipSize);
				continue;
			}

			SceneCaptureMipChain[MipIndex] = UKismetRenderingLibrary::CreateRenderTarget2D(SceneWorld, MipSize, MipSize, RTF_RGBA16f);
			continue;
		}

		SceneCaptureMipChain.Add(UKismetRenderingLibrary::CreateRenderTarget2D(SceneWorld, MipSize, MipSize, RTF_RGBA16f));
	}

	if (!SceneCaptureSRGBMip)
	{
		SceneCaptureSRGBMip = UKismetRenderingLibrary::CreateRenderTarget2D(SceneWorld, ImpostorData->SceneCaptureResolution, ImpostorData->SceneCaptureResolution, RTF_RGBA8_SRGB);
	}
	else if (SceneCaptureSRGBMip->SizeX != ImpostorData->SceneCaptureResolution)
	{
		UKismetRenderingLibrary::ResizeRenderTarget2D(SceneCaptureSRGBMip, ImpostorData->SceneCaptureResolution, ImpostorData->SceneCaptureResolution);
	}
}

void UImpostorRenderTargetsManager::CreateAlphasScratchRenderTargets()
{
	const UImpostorComponentsManager* ComponentsManager = GetManager<UImpostorComponentsManager>();
	const FVector2D RenderTargetSize = ComponentsManager->GetRenderTargetSize();

	const int32 AlphasCount = ImpostorData->ImpostorType == EImpostorLayoutType::TraditionalBillboards ? ComponentsManager->NumHorizontalFrames * ComponentsManager->NumVerticalFrames : 1;
	CombinedAlphas.Reserve(AlphasCount);
	CombinedAlphas.SetNumZeroed(AlphasCount);
	for (int32 Index = 0; Index < AlphasCount; Index++)
	{
		if (CombinedAlphas[Index])
		{
			continue;
		}

		CombinedAlphas[Index] = UKismetRenderingLibrary::CreateRenderTarget2D(SceneWorld, 16, 16, RTF_R8);
	}

	if (!ScratchRenderTarget ||
		ScratchRenderTarget->RenderTargetFormat != RTF_RGBA16f)
	{
		ScratchRenderTarget = UKismetRenderingLibrary::CreateRenderTarget2D(SceneWorld, RenderTargetSize.X, RenderTargetSize.Y, RTF_RGBA16f);
	}
	else if (
		ScratchRenderTarget->SizeX != RenderTargetSize.X ||
		ScratchRenderTarget->SizeY != RenderTargetSize.Y)
	{
		UKismetRenderingLibrary::ResizeRenderTarget2D(ScratchRenderTarget, RenderTargetSize.X, RenderTargetSize.Y);
	}

	if (!BaseColorScratchRenderTarget)
	{
		BaseColorScratchRenderTarget = UKismetRenderingLibrary::CreateRenderTarget2D(SceneWorld, RenderTargetSize.X, RenderTargetSize.Y, RTF_RGBA8_SRGB);
	}
	else if (
		BaseColorScratchRenderTarget->SizeX != RenderTargetSize.X ||
		BaseColorScratchRenderTarget->SizeY != RenderTargetSize.Y)
	{
		UKismetRenderingLibrary::ResizeRenderTarget2D(BaseColorScratchRenderTarget, RenderTargetSize.X, RenderTargetSize.Y);
	}
}

void UImpostorRenderTargetsManager::FillMapsToSave()
{
	MapsToSave = TSet<EImpostorBakeMapType>(ImpostorData->MapsToRender);

	if (ImpostorData->bCombineLightingAndColor)
	{
		MapsToSave.Remove(EImpostorBakeMapType::CustomLighting);
	}

	if (ImpostorData->bCombineNormalAndDepth)
	{
		MapsToSave.Remove(EImpostorBakeMapType::Depth);
	}
}

void UImpostorRenderTargetsManager::SceneCaptureSetup() const
{
	if (!ensure(SceneCaptureMipChain.Num() != 0))
	{
		return;
	}

	UImpostorComponentsManager* ComponentsManager = GetManager<UImpostorComponentsManager>();
	SceneCaptureComponent2D->ShowOnlyComponents = {ComponentsManager->ReferencedMeshComponent};

	TArray<FEngineShowFlagsSetting> ShowFlagsSettings;
	ShowFlagsSettings.Add({"AmbientOcclusion", false});
	ShowFlagsSettings.Add({"Fog", false});
	ShowFlagsSettings.Add({"AtmosphericFog", false});

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	SceneCaptureComponent2D->SetShowFlagSettings(ShowFlagsSettings);
#else
	SceneCaptureComponent2D->ShowFlagSettings = ShowFlagsSettings;
#endif

	SceneCaptureComponent2D->bCaptureEveryFrame = true;
	SceneCaptureComponent2D->CaptureSource = SCS_FinalColorHDR;
	SceneCaptureComponent2D->ProjectionType = ImpostorData->ProjectionType;
	SceneCaptureComponent2D->OrthoWidth = ComponentsManager->ObjectRadius * 2.f;
	SceneCaptureComponent2D->FOVAngle = ImpostorData->CameraFOV;
	SceneCaptureComponent2D->bCaptureOnMovement = false;
	SceneCaptureComponent2D->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	SceneCaptureComponent2D->PostProcessBlendWeight = 1.f;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

void UImpostorRenderTargetsManager::ClearRenderTargets()
{
	for (UTextureRenderTarget2D* RenderTarget : CombinedAlphas)
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(SceneWorld, RenderTarget, FLinearColor::Black);
	}

	UKismetRenderingLibrary::ClearRenderTarget2D(SceneWorld, ScratchRenderTarget, FLinearColor::Black);
	UKismetRenderingLibrary::ClearRenderTarget2D(SceneWorld, BaseColorScratchRenderTarget, FLinearColor::Black);

	for (UTextureRenderTarget2D* MipRenderTarget : SceneCaptureMipChain)
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(SceneWorld, MipRenderTarget, FLinearColor::Black);
	}
	UKismetRenderingLibrary::ClearRenderTarget2D(SceneWorld, SceneCaptureSRGBMip, FLinearColor::Black);

	for (const auto& It : TargetMaps)
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(SceneWorld, It.Value, FLinearColor::Black);
	}
}

void UImpostorRenderTargetsManager::ClearRenderTarget(UTextureRenderTarget2D* RenderTarget) const
{
	UKismetRenderingLibrary::ClearRenderTarget2D(SceneWorld, RenderTarget, FLinearColor::Black);
}

void UImpostorRenderTargetsManager::ResampleRenderTarget(UTextureRenderTarget2D* Source, UTextureRenderTarget2D* Dest) const
{
	const UImpostorMaterialsManager* MaterialsManager = GetManager<UImpostorMaterialsManager>();

	MaterialsManager->ResampleMaterial->SetTextureParameterValue(FName("RT"), Source);

	UKismetRenderingLibrary::ClearRenderTarget2D(SceneWorld, Dest, FLinearColor::Black);
	UKismetRenderingLibrary::DrawMaterialToRenderTarget(SceneWorld, Dest, MaterialsManager->ResampleMaterial);
}

void UImpostorRenderTargetsManager::BakeRenderTargets()
{
	bCapturingFinalColor = false;

	ClearRenderTargets();

	MapsToBake = {};
	MapsToBake.Reserve(ImpostorData->MapsToRender.Num());

	const UImpostorMaterialsManager* MaterialManager = GetManager<UImpostorMaterialsManager>();
	for (const EImpostorBakeMapType MapType : ImpostorData->MapsToRender)
	{
		if (TargetMaps.Contains(MapType) &&
			MaterialManager->HasRenderTypeMaterial(MapType))
		{
			MapsToBake.Add(MapType);
		}
	}

	if (MapsToSave.Contains(EImpostorBakeMapType::BaseColor))
	{
		MapsToBake.Add(EImpostorBakeMapType::BaseColor);
	}

	if (MapsToBake.Num() == 0)
	{
		return;
	}

	NumMapsToBake = MapsToBake.Num();

	ForceTick(true);

	StartSlowTask(NumMapsToBake * GetManager<UImpostorComponentsManager>()->ViewCaptureVectors.Num(), "Capturing impostor map textures...");

	PreparePostProcess(MapsToBake.Pop());
}

TMap<EImpostorBakeMapType, UTexture2D*> UImpostorRenderTargetsManager::SaveTextures()
{
	TMap<EImpostorBakeMapType, UTexture2D*> NewTextures;
	for (const EImpostorBakeMapType TargetMap : MapsToSave)
	{
		ProgressSlowTask("Creating " + GetDefault<UImpostorBakerSettings>()->ImpostorPreviewMapNames[TargetMap].ToString() + " texture...", true);
		UTextureRenderTarget2D* RenderTarget = TargetMaps.FindRef(TargetMap);
		if (!ensure(RenderTarget) ||
			!ensure(RenderTarget->GetResource()))
		{
			continue;
		}

		FString AssetName = ImpostorData->NewTextureName + "_" + GetDefault<UImpostorBakerSettings>()->ImpostorPreviewMapNames[TargetMap].ToString();
		FString PackageName = ImpostorData->GetPackage(AssetName);

		bool bCreatingNewTexture = false;
		UTexture2D* NewTexture = FindObject<UTexture2D>(CreatePackage(*PackageName), *AssetName);
		if (NewTexture)
		{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			RenderTarget->UpdateTexture(NewTexture, EConstructTextureFlags(CTF_Default | CTF_AllowMips), nullptr);
#else
			RenderTarget->UpdateTexture2D(NewTexture, RenderTarget->GetTextureFormatForConversionToTexture2D(), CTF_Default | CTF_AllowMips, nullptr);
#endif
		}
		else
		{
			bCreatingNewTexture = true;
			NewTexture = RenderTarget->ConstructTexture2D(CreatePackage(*PackageName), AssetName, RenderTarget->GetMaskedFlags() | RF_Public | RF_Standalone, CTF_Default | CTF_AllowMips, nullptr);
		}

		if (!ensure(NewTexture))
		{
			continue;
		}

		NewTexture->MarkPackageDirty();

		if (bCreatingNewTexture)
		{
			NewTexture->MipGenSettings = TMGS_FromTextureGroup;
			NewTexture->SRGB = TargetMap == EImpostorBakeMapType::BaseColor;

			if (TargetMap == EImpostorBakeMapType::WorldNormal)
			{
				if (!ImpostorData->bCombineNormalAndDepth ||
					!ImpostorData->MapsToRender.Contains(EImpostorBakeMapType::Depth))
				{
					NewTexture->LODGroup = TEXTUREGROUP_WorldNormalMap;
					NewTexture->CompressionSettings = TC_Normalmap;
				}
			}
		}

		NewTexture->Modify();
		NewTexture->MarkPackageDirty();
		NewTexture->PostEditChange();
		NewTexture->UpdateResource();

		if (bCreatingNewTexture)
		{
			FAssetRegistryModule::AssetCreated(NewTexture);
		}
		NewTextures.Add(TargetMap, NewTexture);
	}

	return NewTextures;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

void UImpostorRenderTargetsManager::PreparePostProcess(const EImpostorBakeMapType TargetMap)
{
	switch (TargetMap)
	{
	default: check(false);
	case EImpostorBakeMapType::BaseColor:
	{
		SceneCaptureComponent2D->CaptureSource = ImpostorData->bUseFinalColorInsteadBaseColor || ImpostorData->ProjectionType == ECameraProjectionMode::Orthographic || bCapturingFinalColor ? SCS_SceneColorHDR : SCS_BaseColor;
		SceneCaptureComponent2D->PostProcessSettings.WeightedBlendables.Array = {};
		SceneCaptureComponent2D->SceneViewExtensions = {};
		Extension = nullptr;
		break;
	}
	case EImpostorBakeMapType::WorldNormal:
	{
		SceneCaptureComponent2D->CaptureSource = SCS_Normal;
		SceneCaptureComponent2D->PostProcessSettings.WeightedBlendables.Array = {};
		SceneCaptureComponent2D->SceneViewExtensions = {};
		Extension = nullptr;
		break;
	}
	case EImpostorBakeMapType::CustomLighting:
	{
		SceneCaptureComponent2D->CaptureSource = SCS_SceneColorHDR;
		SceneCaptureComponent2D->PostProcessSettings.WeightedBlendables.Array = {};
		Extension = FSceneViewExtensions::NewExtension<FLightingViewExtension>(SceneCaptureComponent2D->GetScene());
		SceneCaptureComponent2D->SceneViewExtensions.Add(Extension);
		break;
	}
	case EImpostorBakeMapType::Metallic:
	case EImpostorBakeMapType::Specular:
	case EImpostorBakeMapType::Roughness:
	case EImpostorBakeMapType::Opacity:
	case EImpostorBakeMapType::Subsurface:
	case EImpostorBakeMapType::Depth:
	{
		SceneCaptureComponent2D->CaptureSource = SCS_FinalColorLDR;
		if (UMaterialInterface* Material = GetManager<UImpostorMaterialsManager>()->GetRenderTypeMaterial(TargetMap))
		{
			Material->EnsureIsComplete();
			SceneCaptureComponent2D->PostProcessSettings.WeightedBlendables.Array = { FWeightedBlendable(1.f, Material) };
		}
		else
		{
			SceneCaptureComponent2D->PostProcessSettings.WeightedBlendables.Array = {};
		}
		SceneCaptureComponent2D->SceneViewExtensions = {};
		Extension = nullptr;
		break;
	}
	}

	CurrentMap = TargetMap;
	FramesBeforeCapture = 5;
}

void UImpostorRenderTargetsManager::CaptureImposterGrid()
{
	SceneCaptureComponent2D->TextureTarget = CurrentMap == EImpostorBakeMapType::BaseColor && !bCapturingFinalColor ? SceneCaptureSRGBMip : SceneCaptureMipChain[0];
	FString MapTypeString = GetDefault<UImpostorBakerSettings>()->ImpostorPreviewMapNames[CurrentMap].ToString();
	if (bCapturingFinalColor &&
		CurrentMap == EImpostorBakeMapType::BaseColor)
	{
		MapTypeString = "Final Color (Opacity)";
		ResampleRenderTarget(TargetMaps[CurrentMap], BaseColorScratchRenderTarget);
		ClearRenderTarget(TargetMaps[CurrentMap]);
	}

	const FString Message = "Baking impostor " + MapTypeString + " map... [" + LexToString(NumMapsToBake - MapsToBake.Num()) + " / " + LexToString(NumMapsToBake) + "]";

	const UImpostorComponentsManager* ComponentsManager = GetManager<UImpostorComponentsManager>();
	const TArray<FVector>& ViewCaptureVectors = ComponentsManager->ViewCaptureVectors;

	const int32 ForceEvery = FMath::RoundFromZero(NumMapsToBake * ViewCaptureVectors.Num() / 100.f);

	for (int32 Index = 0; Index < ViewCaptureVectors.Num(); Index++)
	{
		const FVector& Vector = ViewCaptureVectors[Index];

		ProgressSlowTask(Message, Index % ForceEvery == 0);

		FVector WorldLocation = ComponentsManager->GetBounds().Origin;
		WorldLocation += Vector * (ImpostorData->ProjectionType == ECameraProjectionMode::Type::Orthographic ? (ComponentsManager->ObjectRadius * 1.25f) : ImpostorData->CameraDistance);
		SceneCaptureComponent2D->SetWorldLocation(WorldLocation);
		SceneCaptureComponent2D->SetWorldRotation((Vector * -1.f).ToOrientationRotator());

		GetManager<UImpostorMaterialsManager>()->UpdateDepthMaterialData(Vector);

		SceneWorld->SendAllEndOfFrameUpdates();
		SceneCaptureComponent2D->UpdateSceneCaptureContents(SceneWorld->Scene);

		// Lower mips are necessary for distance field alpha and mesh cutouts
		if (ImpostorData->bUseDistanceFieldAlpha ||
			ImpostorData->bUseMeshCutout)
		{
			if (CurrentMap == EImpostorBakeMapType::BaseColor &&
				bCapturingFinalColor)
			{
				for (int32 MipIndex = 1; MipIndex < SceneCaptureMipChain.Num(); MipIndex++)
				{
					UKismetRenderingLibrary::ClearRenderTarget2D(SceneWorld, SceneCaptureMipChain[MipIndex], FLinearColor::Black);
					ResampleRenderTarget(SceneCaptureMipChain[MipIndex - 1], SceneCaptureMipChain[MipIndex]);
				}
			}
		}

		DrawSingleFrame(Index);
	}
}

void UImpostorRenderTargetsManager::DrawSingleFrame(const int32 VectorIndex)
{
	const FVector2D NumFrames(GetManager<UImpostorComponentsManager>()->NumHorizontalFrames, GetManager<UImpostorComponentsManager>()->NumVerticalFrames);
	UTextureRenderTarget2D* RenderTarget = TargetMaps[CurrentMap];

	UCanvas* Canvas;
	FVector2D Size;
	FDrawToRenderTargetContext Context;

	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(SceneWorld, RenderTarget, Canvas, Size, Context);

	UMaterialInstanceDynamic* TargetMaterial = GetManager<UImpostorMaterialsManager>()->GetSampleMaterial(CurrentMap);
	Canvas->K2_DrawMaterial(
		TargetMaterial,
		Size / NumFrames * FVector2D(VectorIndex % FMath::FloorToInt(NumFrames.X), FMath::Floor(VectorIndex / NumFrames.X)),
		Size / NumFrames,
		FVector2D::Zero(),
		FVector2D::One(),
		0.f,
		FVector2D(0.5f, 0.5f));

	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(SceneWorld, Context);

	if (CurrentMap == EImpostorBakeMapType::BaseColor)
	{
		// Accumulate Alphas for Mesh Cutout
		UKismetRenderingLibrary::DrawMaterialToRenderTarget(SceneWorld, CombinedAlphas[ImpostorData->ImpostorType == EImpostorLayoutType::TraditionalBillboards ? VectorIndex : 0], GetManager<UImpostorMaterialsManager>()->AddAlphasMaterial);
	}
}

void UImpostorRenderTargetsManager::FinalizeBaking()
{
	CustomCompositing();

	SceneCaptureComponent2D->TextureTarget = nullptr;

	EndSlowTask();

	ForceTick(false);

	CurrentMap = EImpostorBakeMapType::None;

	if (ImpostorData->bUseMeshCutout)
	{
		GetManager<UImpostorProceduralMeshManager>()->Update();
	}
}

void UImpostorRenderTargetsManager::CustomCompositing() const
{
	if (ImpostorData->MapsToRender.Contains(EImpostorBakeMapType::BaseColor))
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(SceneWorld, ScratchRenderTarget, FLinearColor::Black);
		if (UTextureRenderTarget2D* RenderTarget = TargetMaps.FindRef(EImpostorBakeMapType::BaseColor))
		{
			ResampleRenderTarget(RenderTarget, ScratchRenderTarget);
			UKismetRenderingLibrary::ClearRenderTarget2D(SceneWorld, RenderTarget, FLinearColor::Black);
			UKismetRenderingLibrary::DrawMaterialToRenderTarget(SceneWorld, RenderTarget, GetManager<UImpostorMaterialsManager>()->AddAlphaFromFinalColorMaterial);
		}
	}

	if (ImpostorData->bCombineNormalAndDepth &&
		ImpostorData->MapsToRender.Contains(EImpostorBakeMapType::WorldNormal) &&
		ImpostorData->MapsToRender.Contains(EImpostorBakeMapType::Depth))
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(SceneWorld, ScratchRenderTarget, FLinearColor::Black);
		if (UTextureRenderTarget2D* RenderTarget = TargetMaps.FindRef(EImpostorBakeMapType::WorldNormal))
		{
			ResampleRenderTarget(RenderTarget, ScratchRenderTarget);
			UKismetRenderingLibrary::ClearRenderTarget2D(SceneWorld, RenderTarget, FLinearColor::Black);
			UKismetRenderingLibrary::DrawMaterialToRenderTarget(SceneWorld, RenderTarget, GetManager<UImpostorMaterialsManager>()->CombinedNormalsDepthMaterial);
		}
	}

	if (ImpostorData->bCombineLightingAndColor &&
		ImpostorData->MapsToRender.Contains(EImpostorBakeMapType::BaseColor) &&
		ImpostorData->MapsToRender.Contains(EImpostorBakeMapType::CustomLighting))
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(SceneWorld, BaseColorScratchRenderTarget, FLinearColor::Black);
		if (UTextureRenderTarget2D* RenderTarget = TargetMaps.FindRef(EImpostorBakeMapType::BaseColor))
		{
			ResampleRenderTarget(RenderTarget, BaseColorScratchRenderTarget);
			UKismetRenderingLibrary::ClearRenderTarget2D(SceneWorld, RenderTarget, FLinearColor::Black);
			UKismetRenderingLibrary::DrawMaterialToRenderTarget(SceneWorld, RenderTarget, GetManager<UImpostorMaterialsManager>()->BaseColorCustomLightingMaterial);
		}
	}
}