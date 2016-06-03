#include "OVRLipSyncPrivatePCH.h"
#include "VisemeGenerationWorker.h"

//General Log
//DEFINE_LOG_CATEGORY(OVRLipSyncPlugin);

FVisemeGenerationWorker::FVisemeGenerationWorker() {}


FVisemeGenerationWorker::~FVisemeGenerationWorker() {
	delete Thread;
	Thread = NULL;
}

void FVisemeGenerationWorker::ShutDown() {
	Stop();
	Thread->WaitForCompletion();
	delete Thread;
}

bool FVisemeGenerationWorker::Init() {


	if (!Manager) {
		ClientMessage(FString(TEXT("Speech Recognition Thread failed to start")));
		InitSuccess = false;
		return InitSuccess;
	}

	// Initialisation:
	VoiceCapture = FVoiceModule::Get().CreateVoiceCapture();
	if (!VoiceCapture->Start())
	{
		ClientMessage(FString(TEXT("Failed to open audio device")));
		InitSuccess = false;
		return InitSuccess;
	}

	InitSuccess = true;

	return InitSuccess;
}

uint32 FVisemeGenerationWorker::Run() {

	if (!VoiceCapture->Start())
	{
		ClientMessage(FString(TEXT("Failed to start recording")));
		return 2;
	}

	if (Manager->IsInitialized() != Manager->ovrLipSyncSuccess)
	{
		ClientMessage(FString(TEXT("OVR Lip Sync not initialized")));
		return 3;
	}
	while (StopTaskCounter.GetValue() == 0) 
	{
		// Capturing samples:
		uint32 bytesAvailable = 0;
		EVoiceCaptureState::Type captureState = VoiceCapture->GetCaptureState(bytesAvailable);
		if (captureState == EVoiceCaptureState::Ok && bytesAvailable > 0)
		{
			uint8 buf[2048];
			memset(buf, 0, 2048);
			uint32 readBytes = 0;
			VoiceCapture->GetVoiceData(buf, 1024, readBytes);

			uint32 samples = readBytes / 2;
			float* sampleBuf = new float[samples];

			int16_t sample;
			for (uint32 i = 0; i < samples; i++)
			{
				sample = (buf[i * 2 + 1] << 8) | buf[i * 2];
				sampleBuf[i] = float(sample) / 32768.0f;
			}

			// Do fun stuff here
			uint32 Flags = 0;
			int FrameDelay = 0;
			int FrameNumber = 0;
			TArray<float> Visemes;


			Manager->ProcessFrameExternal(sampleBuf, (ovrLipSyncFlag)Flags);

			Manager->GetFrameInfo(&FrameNumber, &FrameDelay, &Visemes);

			Manager->VisemeGenerated_method(FrameNumber, FrameDelay, Visemes);

			delete[] sampleBuf;
		}
	}

	VoiceCapture->Stop();

	return 0;
}

void FVisemeGenerationWorker::Stop() {
	StopTaskCounter.Increment();
}

bool FVisemeGenerationWorker::StartThread(AVisemeGenerationActor* manager) {
	Manager = manager;
	
	int32 threadIdx = ILipSync::Get().GetInstanceCounter();
	FString threadName = FString("FVisemeGenerationWorker:") + FString::FromInt(threadIdx);
	InitSuccess = true;
	Thread = FRunnableThread::Create(this, *threadName, 0U, TPri_Highest);

	return InitSuccess;
}

void FVisemeGenerationWorker::ClientMessage(FString text) {
	UE_LOG(OVRLipSyncPluginLog, Log, TEXT("%s"), *text);
}
