#include "HydraPluginPrivatePCH.h"

#include "IHydraPlugin.h"
#include "IMotionController.h"

#include "HydraDataDelegate.h"
#include "HydraSingleController.h"
#include "SlateBasics.h"
#include "IPluginManager.h"
#include "HydraComponent.h"

#include <iostream>
#include <stdexcept>
#include <vector>
#include <sixense.h>
#include <windows.h>

#define LOCTEXT_NAMESPACE "OVRLipSyncPlugin"
#define PLUGIN_VERSION "0.8.8"
DEFINE_LOG_CATEGORY_STATIC(OVRLipSyncPluginLog, Log, All);

//Private API - This is where the magic happens

// Error codes that may return from Lip-Sync engine
enum ovrLipSyncError
{
	Unknown = -2200,	//< An unknown error has occurred
	CannotCreateContext = -2201, 	//< Unable to create a context
	InvalidParam = -2202,	//< An invalid parameter, e.g. NULL pointer or out of range
	BadSampleRate = -2203,	//< An unsupported sample rate was declared
	MissingDLL = -2204,	//< The DLL or shared library could not be found
	BadVersion = -2205,	//< Mismatched versions between header and libs
	UndefinedFunction = -2206	//< An undefined function 
};

// Various visemes
enum ovrLipSyncViseme
{
	sil,
	PP,
	FF,
	TH,
	DD,
	kk,
	CH,
	SS,
	nn,
	RR,
	aa,
	E,
	ih,
	oh,
	ou,
	Count
};

/// Flags
enum ovrLipSyncFlag
{
	None = 0x0000,
	DelayCompensateAudio = 0x0001

};

// Enum for sending lip-sync engine specific signals
enum ovrLipSyncSignals
{
	VisemeOn,
	VisemeOff,
	VisemeAmount,
	VisemeSmoothing,
	Count
};

// Enum for provider context to create
enum ovrLipSyncContextProvider
{
	Main,
	Other
};

//DLL import definition
typedef int (*dll_ovrlipsyncInitialize)(int, int);
typedef void (*dll_ovrlipsyncShutdown)(void);
typedef int (*dll_ovrlipsyncGetVersion)(&int, &int, &int);
typedef int (*dll_ovrlipsyncCreateContext)(&unsigned int, int);
typedef int (*dll_ovrlipsyncDestroyContext)(unsigned int);
typedef int (*dll_ovrlipsyncResetContext)(unsigned int);
typedef int (*dll_ovrlipsyncSendSignal)(unsigned int, int, int, int);
typedef int (*dll_ovrlipsyncProcessFrame)(unsigned int, float[], int, &int, &int, float[], int);
typedef int (*dll_ovrlipsyncProcessFrameInterleaved)(unsigned int, float[], int, &int, &int, float[], int);

dll_ovrlipsyncInitialize OVRLipSyncInit;
dll_ovrlipsyncShutdown OVRLipSyncShutdown;
dll_ovrlipsyncGetVersion OVRLipSyncGetVersion;
dll_ovrlipsyncCreateContext OVRLipSyncCreateContext;
dll_ovrlipsyncDestroyContext OVRLipSyncDestroyContext;
dll_ovrlipsyncResetContext OVRLipSyncResetContext;
dll_ovrlipsyncSendSignal OVRLipSyncSendSignal;
dll_ovrlipsyncProcessFrame OVRLipSyncProcessFrame;
dll_ovrlipsyncProcessFrameInterleaved OVRLipSyncProcessFrameInterleaved;


typedef int (*dll_sixenseInit)(void);
typedef int (*dll_sixenseExit)(void);
typedef int (*dll_sixenseGetAllNewestData)(sixenseAllControllerData *);

dll_sixenseInit HydraInit;
dll_sixenseExit HydraExit;
dll_sixenseGetAllNewestData HydraGetAllNewestData;

class HydraUtilityTimer
{
	int64 TickTime = 0;
	int64 TockTime = 0;
public:
	HydraUtilityTimer()
	{
		tick();
	}

	double unixTimeNow()
	{
		FDateTime timeUtc = FDateTime::UtcNow();
		return timeUtc.ToUnixTimestamp() * 10000 + timeUtc.GetMillisecond();
	}

	void tick()
	{
		TickTime = unixTimeNow();
	}

	//return time elapsed in seconds
	float tock()
	{
		TockTime = unixTimeNow();
		return (TockTime - TickTime)/1000.f;
	}
};

//UE v4.6 IM event wrappers
bool EmitKeyUpEventForKey(FKey key, int32 user, bool repeat)
{
	FKeyEvent KeyEvent(key, FSlateApplication::Get().GetModifierKeys(), user, repeat, 0, 0);
	return FSlateApplication::Get().ProcessKeyUpEvent(KeyEvent);
}

bool EmitKeyDownEventForKey(FKey key, int32 user, bool repeat)
{
	FKeyEvent KeyEvent(key, FSlateApplication::Get().GetModifierKeys(), user, repeat, 0, 0);
	return FSlateApplication::Get().ProcessKeyDownEvent(KeyEvent);
}

bool EmitAnalogInputEventForKey(FKey key, float value, int32 user, bool repeat)
{
	FAnalogInputEvent AnalogInputEvent(key, FSlateApplication::Get().GetModifierKeys(), user, repeat, 0, 0, value);
	return FSlateApplication::Get().ProcessAnalogInputEvent(AnalogInputEvent);
}

//Collector class contains all the data captured from .dll and delegate data will point to this structure (allDataUE and historicalDataUE).
class DataCollector
{
public:
	DataCollector()
	{
		allData = new sixenseAllControllerData;
		allDataUE = new sixenseAllControllerDataUE;
		historicalDataUE = new sixenseAllControllerDataUE[10];
	}
	~DataCollector()
	{
		delete allData;
		delete allDataUE;
		delete[] historicalDataUE;
	}

	sixenseControllerDataUE ConvertData(sixenseControllerData* data, FVector offset = FVector(0,0,0))
	{
		sixenseControllerDataUE converted;

		//Convert Sixense Axis to Unreal: UnrealX = - SixenseZ   UnrealY = SixenseX   UnrealZ = SixenseY
		converted.rawPosition = FVector(-data->pos[2] / 10, data->pos[0] / 10, data->pos[1] / 10);					//converted to cm from mm
		converted.position = converted.rawPosition + offset;							
		converted.quat = FQuat(data->rot_quat[2], -data->rot_quat[0], -data->rot_quat[1], data->rot_quat[3]);		//converted & rotation values inverted
		converted.rotation = FRotator(converted.quat);																//convert once and re-use in blueprints
		converted.joystick = FVector2D(data->joystick_x, data->joystick_y);
		converted.trigger = data->trigger;
		converted.buttons = data->buttons;
		converted.sequence_number = data->sequence_number;
		converted.firmware_revision = data->firmware_revision;
		converted.hardware_revision = data->hardware_revision;
		converted.packet_type = data->packet_type;
		converted.magnetic_frequency = data->magnetic_frequency;
		converted.enabled = (data->enabled == 1);
		converted.controller_index = data->controller_index;
		converted.is_docked = (data->is_docked != 0);
		converted.which_hand = data->which_hand;
		converted.hemi_tracking_enabled = (data->hemi_tracking_enabled != 0);

		return converted;
	}

	void ConvertAllData(FVector offset = FVector(0, 0, 0))
	{
		allDataUE->enabledCount = 0;

		for (int i = 0; i < MAX_CONTROLLERS_SUPPORTED; i++)
		{
			allDataUE->controllers[i] = ConvertData(&allData->controllers[i], offset);
			if (allDataUE->controllers[i].enabled){
				allDataUE->enabledCount++;
			}
		}
	}

	sixenseAllControllerDataUE* allDataUE;
	sixenseAllControllerData* allData;
	sixenseAllControllerDataUE* historicalDataUE;
};


class FHydraController : public IInputDevice, public IMotionController
{	

public:
	DataCollector *collector;
	HydraDataDelegate* hydraDelegate;
	void* DLLHandle;
	HydraUtilityTimer UtilityTimer;

	/** handler to send all messages to */
	TSharedRef<FGenericApplicationMessageHandler> MessageHandler;
	

	//Init and Runtime
	FHydraController(const TSharedRef< FGenericApplicationMessageHandler >& InMessageHandler)
		: MessageHandler(InMessageHandler)
	{
		UE_LOG(OVRLipSyncPluginLog, Log, TEXT("Attempting to startup Hydra Module, v%s"), TEXT(PLUGIN_VERSION));

		collector = new DataCollector;
		hydraDelegate = new HydraDataDelegate;
		hydraDelegate->HydraLatestData = collector->allDataUE;	//set the delegate latest data pointer
		hydraDelegate->HydraHistoryData = collector->historicalDataUE;

		//This is a fixed relative path, meaning this file needs to exist for the plugin to work, even in shipping build!
		//Todo: fix path lookup when packaged
		//FString DllFilepath = FPaths::ConvertRelativePathToFull(FPaths::Combine(*FPaths::GameDir(),
		//	TEXT("Plugins"), TEXT("HydraPlugin"), TEXT("Binaries/Win64")), TEXT("sixense_x64.dll"));

		//Define Paths for direct dll bind
		FString BinariesRoot = FPaths::Combine(*FPaths::GameDir(), TEXT("Binaries"));
		FString PluginRoot = IPluginManager::Get().FindPlugin("OVRLipSyncPlugin")->GetBaseDir();
		FString PlatformString;
		FString SixenseDLLString;

#if PLATFORM_WINDOWS
#if _WIN64
		//64bit
		SixenseDLLString = FString(TEXT("OVRLipSync.dll"));
		PlatformString = FString(TEXT("Win64"));
#else
		//32bit
		SixenseDLLString = FString(TEXT("OVRLipSync_x86.dll"));
		PlatformString = FString(TEXT("Win32"));
#endif
#else
		UE_LOG(LogClass, Error, TEXT("Unsupported Platform. Oculus Lip Sync Unavailable."));
#endif
		
		FString DllFilepath = FPaths::ConvertRelativePathToFull(FPaths::Combine(*PluginRoot, TEXT("Binaries"), *PlatformString, *SixenseDLLString));

		UE_LOG(OVRLipSyncPluginLog, Log, TEXT("Fetching dll from %s"), *DllFilepath);

		//Check if the file exists, if not, give a detailed log entry why
		if (!FPaths::FileExists(DllFilepath)){
			UE_LOG(OVRLipSyncPluginLog, Error, TEXT("%s File is missing (Did you copy Binaries into project root?)! Hydra Unavailable."), *SixenseDLLString);
			return;
		}

		DLLHandle = NULL;
		DLLHandle = FPlatformProcess::GetDllHandle(*DllFilepath);

		if (!DLLHandle){
			UE_LOG(OVRLipSyncPluginLog, Error, TEXT("GetDllHandle failed, Hydra Unavailable."));
			UE_LOG(OVRLipSyncPluginLog, Error, TEXT("Full path debug: %s."), *DllFilepath);
			return;
		}

		OVRLipSyncInit = (dll_ovrlipsyncInitialize)FPlatformProcess::GetDllExport(DLLHandle, TEXT("ovrLipSyncDll_Initialize"));
		OVRLipSyncShutdown = (dll_ovrlipsyncShutdown)FPlatformProcess::GetDllExport(DLLHandle, TEXT("ovrLipSyncDll_Shutdown"));
		OVRLipSyncGetVersion = (dll_ovrlipsyncGetVersion)FPlatformProcess::GetDllExport(DLLHandle, TEXT("ovrLipSyncDll_GetVersion"));
		OVRLipSyncCreateContext = (dll_ovrlipsyncCreateContext)FPlatformProcess::GetDllExport(DLLHandle, TEXT("ovrLipSyncDll_CreateContext"));
		OVRLipSyncDestroyContext = (dll_ovrlipsyncDestroyContext)FPlatformProcess::GetDllExport(DLLHandle, TEXT("ovrLipSyncDll_DestroyContext"));
		OVRLipSyncResetContext = (dll_ovrlipsyncResetContext)FPlatformProcess::GetDllExport(DLLHandle, TEXT("ovrLipSyncDll_ResetContext"));
		OVRLipSyncSendSignal = (dll_ovrlipsyncSendSignal)FPlatformProcess::GetDllExport(DLLHandle, TEXT("ovrLipSyncDll_SendSignal"));
		OVRLipSyncProcessFrame = (dll_ovrlipsyncProcessFrame)FPlatformProcess::GetDllExport(DLLHandle, TEXT("ovrLipSyncDll_ProcessFrame"));
		OVRLipSyncProcessFrameInterleaved = (dll_ovrlipsyncProcessFrameInterleaved)FPlatformProcess::GetDllExport(DLLHandle, TEXT("ovrLipSyncDll_ProcessFrameInterleaved"));

		HydraInit = (dll_sixenseInit)FPlatformProcess::GetDllExport(DLLHandle, TEXT("sixenseInit"));
		HydraExit = (dll_sixenseExit)FPlatformProcess::GetDllExport(DLLHandle, TEXT("sixenseExit"));
		HydraGetAllNewestData = (dll_sixenseGetAllNewestData)FPlatformProcess::GetDllExport(DLLHandle, TEXT("sixenseGetAllNewestData"));

		collector->allDataUE->available = (HydraInit() == SIXENSE_SUCCESS);

		if (collector->allDataUE->available)
		{
			UE_LOG(OVRLipSyncPluginLog, Log, TEXT("Hydra Available."));

			//Attach all EKeys

			//Misc
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftDocked, LOCTEXT("HydraLeftDocked", "Hydra Left Docked"), FKeyDetails::GamepadKey));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightDocked, LOCTEXT("HydraRightDocked", "Hydra Right Docked"), FKeyDetails::GamepadKey));

			//Left
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftJoystickX, LOCTEXT("HydraLeftJoystickX", "Hydra Left Joystick X"), FKeyDetails::FloatAxis));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftJoystickY, LOCTEXT("HydraLeftJoystickY", "Hydra Left Joystick Y"), FKeyDetails::FloatAxis));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftJoystickClick, LOCTEXT("HydraLeftJoystickClick", "Hydra Left Joystick Click"), FKeyDetails::GamepadKey));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftB1, LOCTEXT("HydraLeftB1", "Hydra Left B1"), FKeyDetails::GamepadKey));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftB2, LOCTEXT("HydraLeftB2", "Hydra Left B2"), FKeyDetails::GamepadKey));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftB3, LOCTEXT("HydraLeftB3", "Hydra Left B3"), FKeyDetails::GamepadKey));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftB4, LOCTEXT("HydraLeftB4", "Hydra Left B4"), FKeyDetails::GamepadKey));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftStart, LOCTEXT("HydraLeftStart", "Hydra Left Start"), FKeyDetails::GamepadKey));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftTrigger, LOCTEXT("HydraLeftTrigger", "Hydra Left Trigger"), FKeyDetails::FloatAxis));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftTriggerClick, LOCTEXT("HydraLeftTriggerClick", "Hydra Left Trigger Click"), FKeyDetails::GamepadKey));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftBumper, LOCTEXT("HydraLeftBumper", "Hydra Left Bumper"), FKeyDetails::GamepadKey));

			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftMotionX, LOCTEXT("HydraLeftMotionX", "Hydra Left Motion X"), FKeyDetails::FloatAxis));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftMotionY, LOCTEXT("HydraLeftMotionY", "Hydra Left Motion Y"), FKeyDetails::FloatAxis));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftMotionZ, LOCTEXT("HydraLeftMotionZ", "Hydra Left Motion Z"), FKeyDetails::FloatAxis));

			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftRotationPitch, LOCTEXT("HydraLeftRotationPitch", "Hydra Left Rotation Pitch"), FKeyDetails::FloatAxis));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftRotationYaw, LOCTEXT("HydraLeftRotationYaw", "Hydra Left Rotation Yaw"), FKeyDetails::FloatAxis));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraLeftRotationRoll, LOCTEXT("HydraLeftRotationRoll", "Hydra Left Rotation Roll"), FKeyDetails::FloatAxis));

			//Right
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightJoystickX, LOCTEXT("HydraRightJoystickX", "Hydra Right Joystick X"), FKeyDetails::FloatAxis));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightJoystickY, LOCTEXT("HydraRightJoystickY", "Hydra Right Joystick Y"), FKeyDetails::FloatAxis));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightJoystickClick, LOCTEXT("HydraRightJoystickClick", "Hydra Right Joystick Click"), FKeyDetails::GamepadKey));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightB1, LOCTEXT("HydraRightB1", "Hydra Right B1"), FKeyDetails::GamepadKey));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightB2, LOCTEXT("HydraRightB2", "Hydra Right B2"), FKeyDetails::GamepadKey));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightB3, LOCTEXT("HydraRightB3", "Hydra Right B3"), FKeyDetails::GamepadKey));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightB4, LOCTEXT("HydraRightB4", "Hydra Right B4"), FKeyDetails::GamepadKey));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightStart, LOCTEXT("HydraRightStart", "Hydra Right Start"), FKeyDetails::GamepadKey));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightTrigger, LOCTEXT("HydraRightTrigger", "Hydra Right Trigger"), FKeyDetails::FloatAxis));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightTriggerClick, LOCTEXT("HydraRightTriggerClick", "Hydra Right Trigger Click"), FKeyDetails::GamepadKey));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightBumper, LOCTEXT("HydraRightBumper", "Hydra Right Bumper"), FKeyDetails::GamepadKey));

			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightMotionX, LOCTEXT("HydraRightMotionX", "Hydra Right Motion X"), FKeyDetails::FloatAxis));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightMotionY, LOCTEXT("HydraRightMotionY", "Hydra Right Motion Y"), FKeyDetails::FloatAxis));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightMotionZ, LOCTEXT("HydraRightMotionZ", "Hydra Right Motion Z"), FKeyDetails::FloatAxis));

			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightRotationPitch, LOCTEXT("HydraRightRotationPitch", "Hydra Right Rotation Pitch"), FKeyDetails::FloatAxis));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightRotationYaw, LOCTEXT("HydraRightRotationYaw", "Hydra Right Rotation Yaw"), FKeyDetails::FloatAxis));
			EKeys::AddKey(FKeyDetails(EKeysHydra::HydraRightRotationRoll, LOCTEXT("HydraRightRotationRoll", "Hydra Right Rotation Roll"), FKeyDetails::FloatAxis));

		}
		else
		{
			UE_LOG(OVRLipSyncPluginLog, Log, TEXT("Hydra Unavailable."));
		}

		//Required calls at init
		IModularFeatures::Get().RegisterModularFeature(GetModularFeatureName(), this);

		//@todo:  fix this.  construction of the controller happens after InitializeMotionControllers(), so we manually insert into the array here.
		GEngine->MotionControllerDevices.AddUnique(this);
	}

#undef LOCTEXT_NAMESPACE


	virtual ~FHydraController()
	{
		int cleanshutdown = HydraExit();

		FPlatformProcess::FreeDllHandle(DLLHandle);

		if (cleanshutdown == SIXENSE_SUCCESS)
		{
			UE_LOG(OVRLipSyncPluginLog, Log, TEXT("Hydra Clean shutdown."));
		}

		delete hydraDelegate;
		delete collector;

		GEngine->MotionControllerDevices.Remove(this);
	}

	virtual void Tick(float DeltaTime) override
	{
		//Update Data History
	}

	virtual void SendControllerEvents() override
	{
		//Use late sampling attached to SendControllerEvents
		DelegateUpdateAllData(-1.f);	//-1 signifies we should use our internal utility timer for elapsed time
		DelegateEventTick();
	}


	virtual ETrackingStatus GetControllerTrackingStatus(const int32 ControllerIndex, const EControllerHand DeviceHand) const
	{
		UHydraSingleController* controller = hydraDelegate->HydraControllerForControllerHand(DeviceHand);

		if (controller != nullptr && !controller->docked)
		{
			return ETrackingStatus::Tracked;
		}
		else
		{
			return ETrackingStatus::NotTracked;
		}
	}

	//Not guaranteed to work atm
	float GetWorldScale() const
	{
		if (GEngine != nullptr && GEngine->GetWorld() != nullptr)
		{
			return GEngine->GetWorld()->GetWorldSettings()->WorldToMeters;
		}
		return 100.f;
	}

	//Hydra only supports one player so ControllerIndex is ignored.
	virtual bool GetControllerOrientationAndPosition(const int32 ControllerIndex, const EControllerHand DeviceHand, FRotator& OutOrientation, FVector& OutPosition) const
	{
		bool RetVal = false;
		
		UHydraSingleController* controller = hydraDelegate->HydraControllerForControllerHand(DeviceHand);

		if (controller != nullptr && !controller->docked)
		{
			OutOrientation = controller->orientation;
			OutPosition = controller->position  * (GetWorldScale() / 100.f);
			RetVal = true;
		}

		return RetVal;
	}

	virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override
	{
		return false;
	}

	void SetChannelValue(int32 UnrealControllerId, FForceFeedbackChannelType ChannelType, float Value) override
	{
	}
	void SetChannelValues(int32 UnrealControllerId, const FForceFeedbackValues& Values) override
	{
	}
	virtual void SetMessageHandler(const TSharedRef< FGenericApplicationMessageHandler >& InMessageHandler) override
	{
		MessageHandler = InMessageHandler;
	}

private:
	//Delegate Private functions
	void DelegateUpdateAllData(float DeltaTime);
	void DelegateCheckEnabledCount(bool* plugNotChecked);
	void DelegateEventTick();
};


//Public API Implementation


/** Delegate Functions, called by plugin to keep data in sync and to emit the events.*/
void FHydraController::DelegateUpdateAllData(float DeltaTime)
{
	//Tick-tock the timer
	float timeElapsedSinceUpdate = UtilityTimer.tock();
	UtilityTimer.tick();

	if (DeltaTime < 0) 
	{
		DeltaTime = timeElapsedSinceUpdate;
	}

	//Get the freshest Data
	int success = HydraGetAllNewestData(collector->allData);
	if (success == SIXENSE_FAILURE){
		UE_LOG(OVRLipSyncPluginLog, Error, TEXT("Hydra Error! Failed to get freshest data."));
		return;
	}
	//if the hydras are unavailable don't try to get more information
	if (!collector->allDataUE->available){
		UE_LOG(OVRLipSyncPluginLog, Log, TEXT("Collector data not available."));
		return;
	}

	//convert and pass the data to the delegate, this step also adds the base offset
	collector->ConvertAllData(hydraDelegate->baseOffset);

	//Update all historical links
	hydraDelegate->HydraHistoryData[9] = hydraDelegate->HydraHistoryData[8];
	hydraDelegate->HydraHistoryData[8] = hydraDelegate->HydraHistoryData[7];
	hydraDelegate->HydraHistoryData[7] = hydraDelegate->HydraHistoryData[6];
	hydraDelegate->HydraHistoryData[6] = hydraDelegate->HydraHistoryData[5];
	hydraDelegate->HydraHistoryData[5] = hydraDelegate->HydraHistoryData[4];
	hydraDelegate->HydraHistoryData[4] = hydraDelegate->HydraHistoryData[3];
	hydraDelegate->HydraHistoryData[3] = hydraDelegate->HydraHistoryData[2];
	hydraDelegate->HydraHistoryData[2] = hydraDelegate->HydraHistoryData[1];
	hydraDelegate->HydraHistoryData[1] = hydraDelegate->HydraHistoryData[0];

	//Add in all the integrated data (acceleration/velocity etc)
	sixenseControllerDataUE* controller;
	sixenseControllerDataUE* previous;

	float DeltaSquared = (DeltaTime*DeltaTime);

	for (int i = 0; i < MAX_CONTROLLERS_SUPPORTED; i++)
	{
		controller = &hydraDelegate->HydraLatestData->controllers[i];
		previous = &hydraDelegate->HydraHistoryData[0].controllers[i];

		//Trigger 
		if (controller->trigger < 0.5)
		{
			controller->trigger_pressed = false;
		}
		else{
			controller->trigger_pressed = true;
		}

		//Calculate Velocity, Acceleration, and angular velocity
		controller->velocity = (controller->position - previous->position) / DeltaTime;
		controller->acceleration = (controller->velocity - previous->velocity) / DeltaTime;
		controller->angular_velocity = FRotator(controller->quat - previous->quat);	//unscaled by delta time	
		controller->angular_velocity = FRotator(controller->angular_velocity.Pitch / DeltaSquared,
			controller->angular_velocity.Yaw / DeltaSquared,
			controller->angular_velocity.Roll / DeltaSquared);	//has to be scaled here to avoid clamping in the quaternion initialization
	}

	//Update the stored data with the integrated data obtained from latest
	hydraDelegate->HydraHistoryData[0] = *hydraDelegate->HydraLatestData;
}

void FHydraController::DelegateCheckEnabledCount(bool* plugNotChecked)
{
	if (!*plugNotChecked) return;

	sixenseAllControllerDataUE* previous = &hydraDelegate->HydraHistoryData[0];
	int32 oldCount = previous->enabledCount;
	int32 count = hydraDelegate->HydraLatestData->enabledCount;
	if (oldCount != count)
	{
		if (count == 2)	//hydra controller number, STEM behavior undefined.
		{
			hydraDelegate->HydraPluggedIn();
			*plugNotChecked = false;
		}
		else if (count == 0)
		{
			hydraDelegate->HydraUnplugged();
			*plugNotChecked = false;
		}
	}
}

//Function used for consistent conversion to input mapping basis
float MotionInputMappingConversion(float AxisValue){
	return FMath::Clamp(AxisValue / 200.f, -1.f, 1.f);
}

/** Internal Tick - Called by the Plugin */
void FHydraController::DelegateEventTick()
{
	sixenseControllerDataUE* controller;
	sixenseControllerDataUE* previous;
	bool plugNotChecked = true;

	//Trigger any delegate events
	for (int i = 0; i < MAX_CONTROLLERS_SUPPORTED; i++)
	{
		controller = &hydraDelegate->HydraLatestData->controllers[i];
		previous = &hydraDelegate->HydraHistoryData[1].controllers[i];

		//Sync any extended data forms such as HydraSingleController, so that the correct instance values are sent and not 1 frame delayed
		hydraDelegate->UpdateControllerReference(controller, i);

		//If it is enabled run through all the event notifications and data integration
		if (controller->enabled)
		{
			//Enable Check
			if (controller->enabled != previous->enabled)
			{
				DelegateCheckEnabledCount(&plugNotChecked);
				hydraDelegate->HydraControllerEnabled(i);

				//Skip this loop, previous state comparisons will be wrong
				continue;
			}

			//Determine Hand to support dynamic input mapping
			bool leftHand = hydraDelegate->HydraWhichHand(i) == 1;

			//Docking
			if (controller->is_docked != previous->is_docked)
			{
				if (controller->is_docked)
				{
					hydraDelegate->HydraDocked(i);
					if (leftHand)
						EmitKeyDownEventForKey(EKeysHydra::HydraLeftDocked, 0, 0);
					else
						EmitKeyDownEventForKey(EKeysHydra::HydraRightDocked, 0, 0);
				}
				else{
					hydraDelegate->HydraUndocked(i);
					if (leftHand)
						EmitKeyUpEventForKey(EKeysHydra::HydraLeftDocked, 0, 0);
					else
						EmitKeyUpEventForKey(EKeysHydra::HydraRightDocked, 0, 0);
				}
			}

			//** Buttons */

			if (controller->trigger != previous->trigger)
			{
				hydraDelegate->HydraTriggerChanged(i, controller->trigger);
				//InputMapping
				if (leftHand)
				{
					EmitAnalogInputEventForKey(EKeysHydra::HydraLeftTrigger, controller->trigger, 0, 0);
					EmitAnalogInputEventForKey(FGamepadKeyNames::MotionController_Left_TriggerAxis, controller->trigger, 0, 0);
				}
				else
				{
					EmitAnalogInputEventForKey(EKeysHydra::HydraRightTrigger, controller->trigger, 0, 0);
					EmitAnalogInputEventForKey(FGamepadKeyNames::MotionController_Right_TriggerAxis, controller->trigger, 0, 0);
				}
				if (controller->trigger_pressed != previous->trigger_pressed)
				{

					if (controller->trigger_pressed)
					{
						hydraDelegate->HydraButtonPressed(i, HYDRA_BUTTON_TRIGGER);
						hydraDelegate->HydraTriggerPressed(i);
						//InputMapping
						if (leftHand)
						{
							EmitKeyDownEventForKey(EKeysHydra::HydraLeftTriggerClick, 0, 0);
							EmitKeyDownEventForKey(FGamepadKeyNames::MotionController_Left_Trigger, 0, 0);
						}
						else
						{
							EmitKeyDownEventForKey(EKeysHydra::HydraRightTriggerClick, 0, 0);
							EmitKeyDownEventForKey(FGamepadKeyNames::MotionController_Right_Trigger, 0, 0);
						}
					}
					else{
						hydraDelegate->HydraButtonReleased(i, HYDRA_BUTTON_TRIGGER);
						hydraDelegate->HydraTriggerReleased(i);
						//InputMapping
						if (leftHand)
						{
							EmitKeyUpEventForKey(EKeysHydra::HydraLeftTriggerClick, 0, 0);
							EmitKeyUpEventForKey(FGamepadKeyNames::MotionController_Left_Trigger, 0, 0);
						}
						else
						{
							EmitKeyUpEventForKey(EKeysHydra::HydraRightTriggerClick, 0, 0);
							EmitKeyUpEventForKey(FGamepadKeyNames::MotionController_Right_Trigger, 0, 0);
						}
					}
				}
			}

			//Bumper
			if ((controller->buttons & SIXENSE_BUTTON_BUMPER) != (previous->buttons & SIXENSE_BUTTON_BUMPER))
			{
				if ((controller->buttons & SIXENSE_BUTTON_BUMPER) == SIXENSE_BUTTON_BUMPER)
				{
					hydraDelegate->HydraButtonPressed(i, HYDRA_BUTTON_BUMPER);
					hydraDelegate->HydraBumperPressed(i);
					//InputMapping
					if (leftHand)
					{
						EmitKeyDownEventForKey(EKeysHydra::HydraLeftBumper, 0, 0);
						EmitKeyDownEventForKey(FGamepadKeyNames::MotionController_Left_Shoulder, 0, 0);
					}
					else
					{
						EmitKeyDownEventForKey(EKeysHydra::HydraRightBumper, 0, 0);
						EmitKeyDownEventForKey(FGamepadKeyNames::MotionController_Right_Shoulder, 0, 0);
					}
				}
				else{
					hydraDelegate->HydraButtonReleased(i, HYDRA_BUTTON_BUMPER);
					hydraDelegate->HydraBumperReleased(i);
					//InputMapping
					if (leftHand)
					{
						EmitKeyUpEventForKey(EKeysHydra::HydraLeftBumper, 0, 0);
						EmitKeyUpEventForKey(FGamepadKeyNames::MotionController_Left_Shoulder, 0, 0);
					}
					else
					{
						EmitKeyUpEventForKey(EKeysHydra::HydraRightBumper, 0, 0);
						EmitKeyUpEventForKey(FGamepadKeyNames::MotionController_Right_Shoulder, 0, 0);
					}
				}
			}

			//B1
			if ((controller->buttons & SIXENSE_BUTTON_1) != (previous->buttons & SIXENSE_BUTTON_1))
			{
				if ((controller->buttons & SIXENSE_BUTTON_1) == SIXENSE_BUTTON_1)
				{
					hydraDelegate->HydraButtonPressed(i, HYDRA_BUTTON_B1);
					hydraDelegate->HydraB1Pressed(i);
					//InputMapping
					if (leftHand)
					{
						EmitKeyDownEventForKey(EKeysHydra::HydraLeftB1, 0, 0);
						EmitKeyDownEventForKey(FGamepadKeyNames::MotionController_Left_FaceButton1, 0, 0);
					}
					else
					{
						EmitKeyDownEventForKey(EKeysHydra::HydraRightB1, 0, 0);
						EmitKeyDownEventForKey(FGamepadKeyNames::MotionController_Right_FaceButton1, 0, 0);
					}
				}
				else{
					hydraDelegate->HydraButtonReleased(i, HYDRA_BUTTON_B1);
					hydraDelegate->HydraB1Released(i);
					//InputMapping
					if (leftHand)
					{
						EmitKeyUpEventForKey(EKeysHydra::HydraLeftB1, 0, 0);
						EmitKeyUpEventForKey(FGamepadKeyNames::MotionController_Left_FaceButton1, 0, 0);
					}
					else
					{
						EmitKeyUpEventForKey(EKeysHydra::HydraRightB1, 0, 0);
						EmitKeyUpEventForKey(FGamepadKeyNames::MotionController_Right_FaceButton1, 0, 0);
					}
				}
			}
			//B2
			if ((controller->buttons & SIXENSE_BUTTON_2) != (previous->buttons & SIXENSE_BUTTON_2))
			{
				if ((controller->buttons & SIXENSE_BUTTON_2) == SIXENSE_BUTTON_2)
				{
					hydraDelegate->HydraButtonPressed(i, HYDRA_BUTTON_B2);
					hydraDelegate->HydraB2Pressed(i);
					//InputMapping
					if (leftHand)
					{
						EmitKeyDownEventForKey(EKeysHydra::HydraLeftB2, 0, 0);
						EmitKeyDownEventForKey(FGamepadKeyNames::MotionController_Left_FaceButton2, 0, 0);
					}
					else
					{
						EmitKeyDownEventForKey(EKeysHydra::HydraRightB2, 0, 0);
						EmitKeyDownEventForKey(FGamepadKeyNames::MotionController_Right_FaceButton2, 0, 0);
					}
				}
				else{
					hydraDelegate->HydraButtonReleased(i, HYDRA_BUTTON_B2);
					hydraDelegate->HydraB2Released(i);
					//InputMapping
					if (leftHand)
					{
						EmitKeyUpEventForKey(EKeysHydra::HydraLeftB2, 0, 0);
						EmitKeyUpEventForKey(FGamepadKeyNames::MotionController_Left_FaceButton2, 0, 0);
					}
					else
					{
						EmitKeyUpEventForKey(EKeysHydra::HydraRightB1, 0, 0);
						EmitKeyUpEventForKey(FGamepadKeyNames::MotionController_Right_FaceButton2, 0, 0);
					}
				}
			}
			//B3
			if ((controller->buttons & SIXENSE_BUTTON_3) != (previous->buttons & SIXENSE_BUTTON_3))
			{
				if ((controller->buttons & SIXENSE_BUTTON_3) == SIXENSE_BUTTON_3)
				{
					hydraDelegate->HydraButtonPressed(i, HYDRA_BUTTON_B3);
					hydraDelegate->HydraB3Pressed(i);
					//InputMapping
					if (leftHand)
					{
						EmitKeyDownEventForKey(EKeysHydra::HydraLeftB3, 0, 0);
						EmitKeyDownEventForKey(FGamepadKeyNames::MotionController_Left_FaceButton3, 0, 0);
					}
					else
					{
						EmitKeyDownEventForKey(EKeysHydra::HydraRightB3, 0, 0);
						EmitKeyDownEventForKey(FGamepadKeyNames::MotionController_Right_FaceButton3, 0, 0);
					}
				}
				else{
					hydraDelegate->HydraButtonReleased(i, HYDRA_BUTTON_B3);
					hydraDelegate->HydraB3Released(i);
					//InputMapping
					if (leftHand)
					{
						EmitKeyUpEventForKey(EKeysHydra::HydraLeftB3, 0, 0);
						EmitKeyUpEventForKey(FGamepadKeyNames::MotionController_Left_FaceButton3, 0, 0);
					}
					else
					{
						EmitKeyUpEventForKey(EKeysHydra::HydraRightB3, 0, 0);
						EmitKeyUpEventForKey(FGamepadKeyNames::MotionController_Right_FaceButton3, 0, 0);
					}
				}
			}
			//B4
			if ((controller->buttons & SIXENSE_BUTTON_4) != (previous->buttons & SIXENSE_BUTTON_4))
			{
				if ((controller->buttons & SIXENSE_BUTTON_4) == SIXENSE_BUTTON_4)
				{
					hydraDelegate->HydraButtonPressed(i, HYDRA_BUTTON_B4);
					hydraDelegate->HydraB4Pressed(i);
					//InputMapping
					if (leftHand)
					{
						EmitKeyDownEventForKey(EKeysHydra::HydraLeftB4, 0, 0);
						EmitKeyDownEventForKey(FGamepadKeyNames::MotionController_Left_FaceButton4, 0, 0);
					}
					else
					{
						EmitKeyDownEventForKey(EKeysHydra::HydraRightB4, 0, 0);
						EmitKeyDownEventForKey(FGamepadKeyNames::MotionController_Right_FaceButton4, 0, 0);
					}
				}
				else{
					hydraDelegate->HydraButtonReleased(i, HYDRA_BUTTON_B4);
					hydraDelegate->HydraB4Released(i);
					//InputMapping
					if (leftHand)
					{
						EmitKeyUpEventForKey(EKeysHydra::HydraLeftB4, 0, 0);
						EmitKeyUpEventForKey(FGamepadKeyNames::MotionController_Left_FaceButton4, 0, 0);
					}
					else
					{
						EmitKeyUpEventForKey(EKeysHydra::HydraRightB4, 0, 0);
						EmitKeyUpEventForKey(FGamepadKeyNames::MotionController_Right_FaceButton4, 0, 0);
					}
				}
			}

			//Start
			if ((controller->buttons & SIXENSE_BUTTON_START) != (previous->buttons & SIXENSE_BUTTON_START))
			{
				if ((controller->buttons & SIXENSE_BUTTON_START) == SIXENSE_BUTTON_START)
				{
					hydraDelegate->HydraButtonPressed(i, HYDRA_BUTTON_START);
					hydraDelegate->HydraStartPressed(i);
					//InputMapping
					if (leftHand)
					{
						EmitKeyDownEventForKey(EKeysHydra::HydraLeftStart, 0, 0);
						EmitKeyDownEventForKey(FGamepadKeyNames::MotionController_Left_FaceButton5, 0, 0);	//map start to button 5
					}
					else
					{
						EmitKeyDownEventForKey(EKeysHydra::HydraRightStart, 0, 0);
						EmitKeyDownEventForKey(FGamepadKeyNames::MotionController_Right_FaceButton5, 0, 0);	//map start to button 5
					}
				}
				else{
					hydraDelegate->HydraButtonReleased(i, HYDRA_BUTTON_START);
					hydraDelegate->HydraStartReleased(i);
					//InputMapping
					if (leftHand)
					{
						EmitKeyUpEventForKey(EKeysHydra::HydraLeftStart, 0, 0);
						EmitKeyUpEventForKey(FGamepadKeyNames::MotionController_Left_FaceButton5, 0, 0);	//map start to button 5
					}
					else
					{
						EmitKeyUpEventForKey(EKeysHydra::HydraRightStart, 0, 0);
						EmitKeyUpEventForKey(FGamepadKeyNames::MotionController_Right_FaceButton5, 0, 0);	//map start to button 5
					}
				}
			}

			//Joystick Click
			if ((controller->buttons & SIXENSE_BUTTON_JOYSTICK) != (previous->buttons & SIXENSE_BUTTON_JOYSTICK))
			{
				if ((controller->buttons & SIXENSE_BUTTON_JOYSTICK) == SIXENSE_BUTTON_JOYSTICK)
				{
					hydraDelegate->HydraButtonPressed(i, HYDRA_BUTTON_JOYSTICK);
					hydraDelegate->HydraJoystickPressed(i);
					//InputMapping
					if (leftHand)
					{
						EmitKeyDownEventForKey(EKeysHydra::HydraLeftJoystickClick, 0, 0);
						EmitKeyDownEventForKey(FGamepadKeyNames::MotionController_Left_FaceButton6, 0, 0);	//map joystick click to button 6
					}
					else
					{
						EmitKeyDownEventForKey(EKeysHydra::HydraRightJoystickClick, 0, 0);
						EmitKeyDownEventForKey(FGamepadKeyNames::MotionController_Right_FaceButton6, 0, 0);	//map joystick click to button 6
					}
				}
				else{
					hydraDelegate->HydraButtonReleased(i, HYDRA_BUTTON_JOYSTICK);
					hydraDelegate->HydraJoystickReleased(i);
					//InputMapping
					if (leftHand)
					{
						EmitKeyUpEventForKey(EKeysHydra::HydraLeftJoystickClick, 0, 0);
						EmitKeyUpEventForKey(FGamepadKeyNames::MotionController_Left_FaceButton6, 0, 0);	//map joystick click to button 6
					}
					else
					{
						EmitKeyUpEventForKey(EKeysHydra::HydraRightJoystickClick, 0, 0);
						EmitKeyUpEventForKey(FGamepadKeyNames::MotionController_Right_FaceButton6, 0, 0);	//map joystick click to button 6
					}
				}
			}

			/** Movement */

			//Joystick
			if (controller->joystick.X != previous->joystick.X ||
				controller->joystick.Y != previous->joystick.Y)
			{
				hydraDelegate->HydraJoystickMoved(i, controller->joystick);
				//InputMapping
				if (leftHand)
				{
					EmitAnalogInputEventForKey(EKeysHydra::HydraLeftJoystickX, controller->joystick.X, 0, 0);
					EmitAnalogInputEventForKey(EKeysHydra::HydraLeftJoystickY, controller->joystick.Y, 0, 0);

					EmitAnalogInputEventForKey(FGamepadKeyNames::MotionController_Left_Thumbstick_X, controller->joystick.X, 0, 0);
					EmitAnalogInputEventForKey(FGamepadKeyNames::MotionController_Left_Thumbstick_Y, controller->joystick.Y, 0, 0);
				}
				else
				{
					EmitAnalogInputEventForKey(EKeysHydra::HydraRightJoystickX, controller->joystick.X, 0, 0);
					EmitAnalogInputEventForKey(EKeysHydra::HydraRightJoystickY, controller->joystick.Y, 0, 0);

					EmitAnalogInputEventForKey(FGamepadKeyNames::MotionController_Right_Thumbstick_X, controller->joystick.X, 0, 0);
					EmitAnalogInputEventForKey(FGamepadKeyNames::MotionController_Right_Thumbstick_Y, controller->joystick.Y, 0, 0);
				}
			}

			//Controller

			if (!controller->is_docked){
				FRotator rotation = FRotator(controller->rotation);

				//If the controller isn't docked, it's moving
				hydraDelegate->HydraControllerMoved(i,
					controller->position, controller->velocity, controller->acceleration,
					rotation, controller->angular_velocity);

				//InputMapping
				if (leftHand)
				{
					//2 meters = 1.0
					EmitAnalogInputEventForKey(EKeysHydra::HydraLeftMotionX, MotionInputMappingConversion(controller->position.X), 0, 0);
					EmitAnalogInputEventForKey(EKeysHydra::HydraLeftMotionY, MotionInputMappingConversion(controller->position.Y), 0, 0);
					EmitAnalogInputEventForKey(EKeysHydra::HydraLeftMotionZ, MotionInputMappingConversion(controller->position.Z), 0, 0);

					EmitAnalogInputEventForKey(EKeysHydra::HydraLeftRotationPitch, rotation.Pitch / 90.f, 0, 0);
					EmitAnalogInputEventForKey(EKeysHydra::HydraLeftRotationYaw, rotation.Yaw / 180.f, 0, 0);
					EmitAnalogInputEventForKey(EKeysHydra::HydraLeftRotationRoll, rotation.Roll / 180.f, 0, 0);
				}
				else
				{
					//2 meters = 1.0
					EmitAnalogInputEventForKey(EKeysHydra::HydraRightMotionX, MotionInputMappingConversion(controller->position.X), 0, 0);
					EmitAnalogInputEventForKey(EKeysHydra::HydraRightMotionY, MotionInputMappingConversion(controller->position.Y), 0, 0);
					EmitAnalogInputEventForKey(EKeysHydra::HydraRightMotionZ, MotionInputMappingConversion(controller->position.Z), 0, 0);

					EmitAnalogInputEventForKey(EKeysHydra::HydraRightRotationPitch, rotation.Pitch / 90.f, 0, 0);
					EmitAnalogInputEventForKey(EKeysHydra::HydraRightRotationYaw, rotation.Yaw / 180.f, 0, 0);
					EmitAnalogInputEventForKey(EKeysHydra::HydraRightRotationRoll, rotation.Roll / 180.f, 0, 0);
				}
			}
		}//end enabled
		else{
			if (controller->enabled != previous->enabled)
			{
				DelegateCheckEnabledCount(&plugNotChecked);
				hydraDelegate->HydraControllerDisabled(i);
			}
		}
	}//end controller for loop
}

//Implementing the module, required
class FHydraPlugin : public IHydraPlugin
{
	FHydraController* controllerReference = nullptr;
	TArray<UHydraPluginComponent*> delegateComponents;
	bool inputDeviceCreated = false;

	virtual TSharedPtr< class IInputDevice > CreateInputDevice(const TSharedRef< FGenericApplicationMessageHandler >& InMessageHandler) override
	{
		controllerReference = new FHydraController(InMessageHandler);
		// Add defered delegates
		for (auto& actorComponent : delegateComponents) {
			controllerReference->hydraDelegate->AddEventDelegate(actorComponent);
			actorComponent->SetDataDelegate(controllerReference->hydraDelegate);
		}
		delegateComponents.Empty();
		inputDeviceCreated = true;

		return TSharedPtr< class IInputDevice >(controllerReference);
	}

	virtual HydraDataDelegate* DataDelegate() override
	{
		return controllerReference->hydraDelegate;
	}

	virtual void DeferedAddDelegate(UHydraPluginComponent* delegate) override
	{
		if (inputDeviceCreated) 
		{
			controllerReference->hydraDelegate->AddEventDelegate(delegate);
			delegate->SetDataDelegate(controllerReference->hydraDelegate);
		}
		else 
		{
			// defer until later
			delegateComponents.Add(delegate);
		}
	}

};

//Second parameter needs to be called the same as the Module name or packaging will fail
IMPLEMENT_MODULE(FHydraPlugin, HydraPlugin)	