#include <OpenMPD.h>
#include <src/EngineThreads.h>
#include <src/ThreadEngine_SyncData.h>
#include <src/BufferManager.h>
#include <src/PrimitiveDescriptors.h>
#include <src/OpenMPD_Context.h>
#include <src/Synchronize/ForceUPS_Sync.h>

void (*_printMessage_OpenMPD) (const char*) = NULL;
void (*_printError_OpenMPD)(const char*)= NULL;
void (*_printWarning_OpenMPD)(const char*)= NULL;
ForceUPS_Sync* syncWithExternalThread = NULL;

//Parameters set during call to setup (not used until Start is called)
OpenMPD::GSPAT_SOLVER version = OpenMPD::GSPAT_SOLVER::NAIVE ;
size_t _memorySizeInBytes;
int numSolverConfigParam = 0;
GSPAT::Solver::ConfigParameter* configParameters;

void OpenMPD::printMessage_OpenMPD(const char* str) {
	void(*aux)(const char*) = _printMessage_OpenMPD;//Atomic read (avoid thread sync issues)
	if (aux) aux(str);
}
void OpenMPD::printError_OpenMPD(const char* str) {
	void(*aux)(const char*) = _printError_OpenMPD;//Atomic read (avoid thread sync issues)
	if (aux) aux(str);
}
void OpenMPD::printWarning_OpenMPD(const char* str) {
	void(*aux)(const char*) = _printWarning_OpenMPD;//Atomic read (avoid thread sync issues)
	if (aux) aux(str);
}

void OpenMPD::RegisterPrintFuncs(void(*p_Message)(const char*), void(*p_Warning)(const char*), void(*p_Error)(const char*)) {
	_printMessage_OpenMPD = p_Message;
	_printError_OpenMPD = p_Error;
	_printWarning_OpenMPD = p_Warning;
	OpenMPD::printMessage_OpenMPD("Got message functions!");
}

static ThreadEngine_SyncData* threadEngine_SyncData=NULL;

bool OpenMPD::Initialize() {
	/*if(threadEngine_SyncData != NULL)
		printMessage_OpenMPD("OpenMPD was already initialized. Command ignored.");	*/
	return true;
}

void OpenMPD::SetupEngine(size_t memorySizeInBytes, OpenMPD::GSPAT_SOLVER v,OpenMPD::IVisualRenderer* renderer, int numConfigParam , GSPAT::Solver::ConfigParameter* configParam ){
	//0. Check if this step was already run:
	if (threadEngine_SyncData != NULL) {
		printWarning_OpenMPD("OpenMPD was already setup. Command ignored. \n If you want to redefine the memory size used, you need to completeley destroy (Stop, Release) the engine first.");	
		return;
	}
	//1. Create shared data structures supporting the rendering engine:
	{
		threadEngine_SyncData = new ThreadEngine_SyncData;
		//Create driver and connect to it
		AsierInho_V2::RegisterPrintFuncs(printMessage_OpenMPD, printMessage_OpenMPD, printMessage_OpenMPD);
		threadEngine_SyncData->driver = AsierInho_V2::createAsierInho();
		threadEngine_SyncData->renderer = renderer;
		version = v; 
		_memorySizeInBytes = memorySizeInBytes;		
	}
	//2. Save config parameters
	numSolverConfigParam = numConfigParam;
	configParameters = configParam;
}

OpenMPD::IPrimitiveUpdater* OpenMPD::StartEngine_TopBottom( cl_uchar FPS_Divider, cl_uint numParallelGeometries, cl_uint topBoardID, cl_uint bottomBoardID, bool forceSync) {
	cl_uint boardIDs[] = { bottomBoardID, topBoardID };
	float matrices[] = {/*bottom*/
						1, 0, 0, 0,
						0, 1, 0, 0,
						0, 0, 1, 0,
						0, 0, 0, 1,	
						/*top*/
						-1, 0, 0, 0,
						0, 1, 0, 0,
						0, 0,-1, 0.24f,
						0, 0, 0, 1,	
	};
	return OpenMPD::StartEngine(FPS_Divider, numParallelGeometries, topBoardID!=0? 2 : 1 , boardIDs, matrices, forceSync);
}


OpenMPD::IPrimitiveUpdater* OpenMPD::StartEngine_SingleBoard(cl_uchar FPS_Divider, cl_uint numParallelGeometries, cl_uint boardID, float* matToWorld, bool forceSync) {
	return OpenMPD::StartEngine(FPS_Divider, numParallelGeometries, 1, &boardID, matToWorld, forceSync);	
}



_OPEN_MPD_ENGINE_Export OpenMPD::IPrimitiveUpdater* OpenMPD::StartEngine( cl_uchar FPS_Divider, cl_uint numParallelGeometries, cl_uint numBoards, cl_uint* boardIDs, float* boardLocationsM4x4, bool forceSync ) {
	if (threadEngine_SyncData == NULL) {
		printError_OpenMPD("OpenMPD::StartEngine:: OpenMPD is being started without being initialized.\nPlease, call OpenMPD::SetupEngine(<Memory_Size>) before running the engine.");
		return NULL;
	}

	//1. Configure underlying running infrastructure:
	{
		threadEngine_SyncData->numGeometries = numParallelGeometries;
		threadEngine_SyncData->newDivider = FPS_Divider;
		threadEngine_SyncData->targetUPS = 40000.0f / FPS_Divider;
		//Connect to boards and configure solver:
		if (threadEngine_SyncData->driver->connect(numBoards, (int*)boardIDs, boardLocationsM4x4))
			OpenMPD::printMessage_OpenMPD("OpenMPD::StartEngine:: Connection to device succeeded!");
	}
	//2. Create and setup solver:
	{		
		switch (version) {		
		case GSPAT_SOLVER::V2:
			GSPAT_V2::RegisterPrintFuncs(printMessage_OpenMPD, printMessage_OpenMPD, printMessage_OpenMPD);
			threadEngine_SyncData->solver = GSPAT_V2::createSolver(threadEngine_SyncData->driver->totalTransducers());
			OpenMPD::printMessage_OpenMPD("OpenMPD::StartEngine:: Engine setup with GSPAT Version 2 (GSPAT_SolverV2.dll)");
			break;
		case GSPAT_SOLVER::IBP:
			GSPAT_IBP::RegisterPrintFuncs(printMessage_OpenMPD, printMessage_OpenMPD, printMessage_OpenMPD);
			threadEngine_SyncData->solver = GSPAT_IBP::createSolver(threadEngine_SyncData->driver->totalTransducers());
			OpenMPD::printMessage_OpenMPD("OpenMPD::StartEngine:: Engine setup with GSPAT Version 1 (IBP - GSPAT_SolverIBP.dll)");
			break;

#ifdef _ADVANCED_SOLVERS
		case GSPAT_SOLVER::BEM:
			GSPAT_BEM::RegisterPrintFuncs(printMessage_OpenMPD, printMessage_OpenMPD, printMessage_OpenMPD);
			threadEngine_SyncData->solver = GSPAT_BEM::createSolver(threadEngine_SyncData->driver->totalTransducers());
			OpenMPD::printMessage_OpenMPD("OpenMPD::StartEngine:: Engine setup with GSPAT Version 1 (IBP - GSPAT_SolverIBP.dll)");
			break;
		case GSPAT_SOLVER::TS:
			GSPAT_TS::RegisterPrintFuncs(printMessage_OpenMPD, printMessage_OpenMPD, printMessage_OpenMPD);
			threadEngine_SyncData->solver = GSPAT_TS::createSolver(threadEngine_SyncData->driver->totalTransducers());
			OpenMPD::printMessage_OpenMPD("OpenMPD::StartEngine:: Engine setup with GSPAT Version 1 (IBP - GSPAT_SolverIBP.dll)");
			break;
#endif
		case GSPAT_SOLVER::NAIVE:
		default:
			GSPAT_Naive::RegisterPrintFuncs(printMessage_OpenMPD, printMessage_OpenMPD, printMessage_OpenMPD);
			threadEngine_SyncData->solver = GSPAT_Naive::createSolver(threadEngine_SyncData->driver->totalTransducers());
			OpenMPD::printMessage_OpenMPD("OpenMPD::StartEngine:: Engine setup with GSPAT Version 0 (Naive - GSPAT_SolverNaive.dll)");
			break;
		}
		//Create joint data structures for direct communication Engine-Solver (Primitives and Buffer Managers)
		struct OpenCL_ExecutionContext * executionContext = (struct OpenCL_ExecutionContext *)threadEngine_SyncData->solver->getSolverContext();
		OpenMPD::OpenMPD_Context::instance().initialize(_memorySizeInBytes / sizeof(float), executionContext, threadEngine_SyncData->renderer);//2Mfloats (64MB?)
		printMessage_OpenMPD("OpenMPD::StartEngine:: OpenMPD started successfully.");
		//Configure solver with callibration data
		float* transducerPositions = new float[threadEngine_SyncData->driver->totalTransducers()* 3], * transducerNormals = new float[threadEngine_SyncData->driver->totalTransducers()* 3], *amplitudeAdjust=new float[threadEngine_SyncData->driver->totalTransducers()];
		int* mappings= new int[threadEngine_SyncData->driver->totalTransducers()], *phaseDelays=new int[threadEngine_SyncData->driver->totalTransducers()], numDiscreteLevels;
		threadEngine_SyncData->driver->readParameters(transducerPositions, transducerNormals, mappings, phaseDelays, amplitudeAdjust, &numDiscreteLevels);
		threadEngine_SyncData->solver->setBoardConfig(transducerPositions, transducerNormals, mappings, phaseDelays, amplitudeAdjust, numDiscreteLevels);
		threadEngine_SyncData->solver->setConfigParameters(numSolverConfigParam, configParameters);
	}
	//2. Setup and run working threads: 
	{
		printMessage_OpenMPD("OpenMPD::StartEngine:: OpenMPD setting up threads.");
		pthread_t engineWritterThread, engineReaderThread;
		//Initialize sync data:
		pthread_mutex_init(&(threadEngine_SyncData->solution_available), NULL);
		pthread_mutex_lock(&(threadEngine_SyncData->solution_available));
		pthread_mutex_init(&(threadEngine_SyncData->mutex_solution_queue), NULL);
		threadEngine_SyncData->running = true;
		printMessage_OpenMPD("OpenMPD::StartEngine:: OpenMPD synchronization resources setup.");
		//Create threads:
		{
			int rc;
			pthread_attr_t attr;
			struct sched_param param;
			rc = pthread_attr_init(&attr);
			rc = pthread_attr_getschedparam(&attr, &param);
			(param.sched_priority)++;
			rc = pthread_attr_setschedparam(&attr, &param);
		}

		threadEngine_SyncData->runningThreads = 2;
		pthread_create(&engineWritterThread, NULL, engineWritter, threadEngine_SyncData);
		pthread_create(&engineReaderThread, NULL, engineReader, threadEngine_SyncData);
		printMessage_OpenMPD("OpenMPD::StartEngine:: OpenMPD threads launched.");
	}
	threadEngine_SyncData->driver->turnTransducersOn();
	printMessage_OpenMPD("OpenMPD::StartEngine:: OpenMPD started successfully.");
	if (forceSync) {
		syncWithExternalThread = new ForceUPS_Sync();
		OpenMPD::OpenMPD_Context::instance().addListener(syncWithExternalThread);
		addEngineWriterListener(syncWithExternalThread);
	}		

	return &(OpenMPD::OpenMPD_Context::primitiveUpdaterInstance());
}

_OPEN_MPD_ENGINE_Export void OpenMPD::updateBoardSeparation(float distance)
{
	//0. Check status
	if (threadEngine_SyncData == NULL) {
		printWarning_OpenMPD("OpenMPD is not setup, so board distance cannot be adjusted.\n Please make sure to call SetupEngine and StartEngine before doing this.\n Command ignored.");
		return ;
	}
	if (threadEngine_SyncData ->driver->totalTransducers()!=512){
		printWarning_OpenMPD("OpenMPD does not seem to be setup in a top-bottom arrangement, so separation cannot be changed. \n Command ignored.");
		return ;
	}
	pthread_mutex_lock(&(threadEngine_SyncData->mutex_solution_queue));
	//1. Apply configuration
	float matBoardToWorld[32] = {	/*bottom*/
								1, 0, 0, 0,
								0, 1, 0, 0,
								0, 0, 1, 0,
								0, 0, 0, 1,	
								/*top*/
								-1, 0, 0, 0,
								0, 1, 0, 0,
								0, 0,-1, distance,
								0, 0, 0, 1,	
	};
	float transducerPositions[512 * 3], transducerNormals[512 * 3], amplitudeAdjust[512];
	int mappings[512], phaseDelays[512], numDiscreteLevels;
	threadEngine_SyncData->driver->updateBoardPositions(matBoardToWorld);
	threadEngine_SyncData->driver->readParameters(transducerPositions, transducerNormals, mappings, phaseDelays, amplitudeAdjust, &numDiscreteLevels);
	threadEngine_SyncData->solver->setBoardConfig(transducerPositions, transducerNormals, mappings, phaseDelays, amplitudeAdjust, numDiscreteLevels);
	//3. Unlock threads.
	pthread_mutex_unlock(&(threadEngine_SyncData->mutex_solution_queue));

}

_OPEN_MPD_ENGINE_Export void OpenMPD::updateBoardLocations(float* boardsToLevitatorOrigin)
{
	//0. Check status
	if (threadEngine_SyncData == NULL) {
		printWarning_OpenMPD("OpenMPD is not setup, so board positions cannot be adjusted.\n Please make sure to call SetupEngine and StartEngine before doing this.\n Command ignored.");
		return ;
	}
	pthread_mutex_lock(&(threadEngine_SyncData->mutex_solution_queue));
	//1. Apply configuration
	int numTransducers = threadEngine_SyncData->driver->totalTransducers();
	float* transducerPositions=new float [numTransducers* 3], *transducerNormals=new float [numTransducers* 3], *amplitudeAdjust=new float [numTransducers];
	int* mappings=new int[numTransducers], *phaseDelays=new int[numTransducers], numDiscreteLevels;
	threadEngine_SyncData->driver->updateBoardPositions(boardsToLevitatorOrigin);
	threadEngine_SyncData->driver->readParameters(transducerPositions, transducerNormals, mappings, phaseDelays, amplitudeAdjust, &numDiscreteLevels);
	threadEngine_SyncData->solver->setBoardConfig(transducerPositions, transducerNormals, mappings, phaseDelays, amplitudeAdjust, numDiscreteLevels);
	//3. Unlock threads.
	pthread_mutex_unlock(&(threadEngine_SyncData->mutex_solution_queue));

}


_OPEN_MPD_ENGINE_Export void OpenMPD::setupFPS_Divider(unsigned char FPSDivider)
{
	threadEngine_SyncData->newDivider = FPSDivider;
}

_OPEN_MPD_ENGINE_Export void OpenMPD::setupPhaseOnly(bool phaseOnly)
{
	threadEngine_SyncData->phaseOnly= phaseOnly;
}

_OPEN_MPD_ENGINE_Export void OpenMPD::setupHardwareSync(bool useHardwareSync) {
	threadEngine_SyncData->hardwareSync = useHardwareSync;
}

bool  OpenMPD::StopEngine() {
	if (threadEngine_SyncData == NULL) {
		printWarning_OpenMPD("OpenMPD is not running, so it cannot be stopped either. Command ignored.");
		return false;
	}
	//Disable force sync to allow threads to finish without affecting rach other.
	if (syncWithExternalThread) 
		syncWithExternalThread->disable();			
	//Wait for threads to pick on the message and finish their executions
	threadEngine_SyncData->running = false;
	while (threadEngine_SyncData->runningThreads > 0) {
			Sleep(100);
	}
	//Remove force sync from related modules and delete it (i.e. start engine might decide force sync is not required)	
	if (syncWithExternalThread) {
		OpenMPD::OpenMPD_Context::instance().removeListener(syncWithExternalThread);
		removeEngineWriterListener(syncWithExternalThread);
		delete syncWithExternalThread;
		syncWithExternalThread = NULL;
	}
	pthread_mutex_destroy(&(threadEngine_SyncData->solution_available));
	pthread_mutex_destroy(&(threadEngine_SyncData->mutex_solution_queue));
	threadEngine_SyncData->driver->turnTransducersOff();
	//void removeEngineWriterListener(EngineWriterListener* e);
	printMessage_OpenMPD("OpenMPD stopped.");	
	return true;

}
	
bool OpenMPD::Release() {
	//0. Trivial case:
	if (threadEngine_SyncData == NULL){
		printMessage_OpenMPD("OpenMPD released. There was nothing setup, so no resources needed to be deallocated. ");
		return true;
	}
	//1. Check status:
	if(threadEngine_SyncData->running != false || threadEngine_SyncData->runningThreads != 0){
		printWarning_OpenMPD("OpenMPD cannot be released because some threads are still running.\n Calling StopEngine before releasing... ");
		StopEngine();
	}
	//2. Destroy all related data structures: 
	OpenMPD_Context::instance().releaseAllResources();	
	threadEngine_SyncData->driver->turnTransducersOff();
	threadEngine_SyncData->driver->disconnect();
	delete threadEngine_SyncData->driver;
	delete threadEngine_SyncData->solver;
	delete threadEngine_SyncData;
	threadEngine_SyncData = NULL;
	printMessage_OpenMPD("OpenMPD Released. All resources destroyed.");	
	return true;
}

int OpenMPD::GetCurrentPosIndex() {
	OpenMPD::OpenMPD_Context& pm = OpenMPD::OpenMPD_Context::instance();
	return pm.getCurrentPosIndex();
}
