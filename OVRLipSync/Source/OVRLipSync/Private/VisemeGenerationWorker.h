#pragma once
#include "Voice.h"
#include "OnlineSubsystemUtils.h"
#include "VisemeGenerationActor.h"

class AVisemeGenerationActor;

using namespace std;

class FVisemeGenerationWorker :public FRunnable
{

private:

	TSharedPtr<class IVoiceCapture> VoiceCapture;

	//Thread
	FRunnableThread* Thread;

	//Pointer to our manager
	AVisemeGenerationActor* Manager;

	//Thread safe counter 
	FThreadSafeCounter StopTaskCounter;

	//Language
	char* langStr = NULL;

	bool InitSuccess = false;

public:
	FVisemeGenerationWorker();
	virtual ~FVisemeGenerationWorker();

	//FRunnable interface
	virtual bool Init();
	virtual void Stop();
	virtual uint32 Run();

	bool StartThread(AVisemeGenerationActor* manager);

	static void ClientMessage(FString txt);

	void ShutDown();

};

