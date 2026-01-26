#ifndef _LXDeviceAPI_H
#define _LXDeviceAPI_H

// Predefined Message Type ID for SetMessageDevice
#define MSGTYPEID0_DEVICE_LXDAPI 0
#define MSGTYPEID1_DEVICE_LXDAPI 1
#define MSGTYPEID2_DEVICE_LXDAPI 2
#define MSGTYPEID3_DEVICE_LXDAPI 3
#define MSGTYPEID4_DEVICE_LXDAPI 4
#define MSGTYPEID5_DEVICE_LXDAPI 5
#define MSGTYPEID6_DEVICE_LXDAPI 6
#define MSGTYPEID7_DEVICE_LXDAPI 7
#define MSGTYPEID8_DEVICE_LXDAPI 8
#define MSGTYPEID9_DEVICE_LXDAPI 9

// Predefined Message Type ID for SetMessageAPI
#define MSGTYPEID0_API_LXDAPI 10
#define MSGTYPEID1_API_LXDAPI 11
#define MSGTYPEID2_API_LXDAPI 12
#define MSGTYPEID3_API_LXDAPI 13
#define MSGTYPEID4_API_LXDAPI 14
#define MSGTYPEID5_API_LXDAPI 15
#define MSGTYPEID6_API_LXDAPI 16
#define MSGTYPEID7_API_LXDAPI 17
#define MSGTYPEID8_API_LXDAPI 18
#define MSGTYPEID9_API_LXDAPI 19

// struct for data abstracted
typedef struct _LXDataAbs_LXDAPI	// 
{
	unsigned int	NumAui;			// Aui[]의 배열크기.
	unsigned int	NumAd;			// Ad[]의 배열크기.
	unsigned int	NumX;
	unsigned int	NumY;
	unsigned int	NumZ;
	unsigned int*	Aui;			// Aui[NumAui]				
	unsigned int*	Aui_X;
	unsigned int*	Aui_Y;
	unsigned int*	Aui_XY;
	unsigned int*	Aui_XYZ;
	double*			Ad;				// Ad[NumAd]
	double*			Ad_X;
	double*			Ad_Y;
	double*			Ad_XY;
	double*			Ad_XYZ;
}ST_ABS_LXDAPI,*LPST_ABS_LXDAPI;

// struct for stream.
typedef struct _stStreamData_LXDAPI
{
	unsigned int	LXDeviceID;			// 장치아이디(장치모델과 1:1대응). 본 스트림 데이터가 어떤 장치에서 발생한 것인지 표식.
	unsigned int	DeviceHandlingID;	// 장치를 개별적으로 식별하는 고유아이디. (LXDeviceID가 동일한 장치 2개 이상과 통신시 장치별로 식별가능한 아이디.)
	unsigned int	StreamID;			// 본 스트림데이터 아이디.(구조체 변수 1개당 StreamDataID  1개 대응)
	unsigned int	StreamType;			// 
	unsigned int	NumSampleReturn;	// 스트림으로 전송되는 각 채널당 샘플링수량. 
	unsigned int	MaxSampleBuffer;	// 스트림 데이터 버퍼가 저장가능한 최대 샘플수량.
	unsigned int	NumSampleBuffer;	// 버퍼에 남아있는 샘플링수량. - 현재 스트림 전송시점에 버퍼에 남아있는 샘플링 수량임.
	double			mTimeInterSample;	// 샘플사이의 시간격. 단위:밀리초.
	unsigned int	Wave_NumChannel;	// 스트림으로 전송되는 파형채널의 수량.
	unsigned int	Event_NumChannel;	// 스트림으로 전송되는 이벤트채널의 수량..
	unsigned int*	Wave_Source_C;		// wave 각 채널별 전송되는 값의 의미. eeg, ppg 등 측정대상이 무엇인지 표식.
	unsigned int*	Wave_Polarity_C;	// wave 각 채널의 극성특성.2 양극성,  1 단극성.
	unsigned int*	Event_Source_C;		// event 각 채널별로 전송되는 이벤트 소스 : 키보드, 앱에서함수호출에 의한것, 장치에서 하드웨어적으로 전송된것. 현재 3종.
	unsigned int*	Wave_Dimension_C;	// wave 각 채널로 전송되는 값의 단위. 배열 사이즈 동적 설정된다. 숫자-단위 대응표 보고 파악할것. V,A,SEC 등이 있음.
	unsigned int*	Event_StreamData_CS;// event 스트림 데이터를 받을 1차원 배열. 배열크기 = Stream_NumSamples*Num_ChannelMark
	int*			Wave_DimScale_C;	// 단위 앞의 승수. M, G등.
	double*			Wave_MaxAmplitude_C;// wave 각 채널당 전송되는 값이 가질수 있는 최대 진폭. 배열사이즈는 동적 설정된다. MaxAmplitudeWave[Num_ChannelWave]
	double*			Wave_StreamData_CS;	// wave 스트림데이터를 받을 1차원 배열. 배열사이즈는 Stream_NumSamples 에 종속되어 동적으로 설정(배열크기 = Stream_NumSamples*Num_ChannelWave)되어야 하며, 동적메모리 할당은 API내부에서 이뤄진다. 채널인덱스 i, 샘플인덱스 j 에 접근하는법 Sream[j+i*Stream_NumSample]

	LPST_ABS_LXDAPI	pst_Abs;			// abstracted.- don't care.

}ST_STREAMDATA_LXDAPI;


/// LXDeviceAPI.DLL Exported Functions. 

extern "C" __declspec(dllimport) int OpenApi_LXDeviceAPI(int api_window, int api_selfupdate, int mode);
extern "C" __declspec(dllimport) int SetMessageAPI_LXDeviceAPI(int msgtype_id, HWND hwnd_msgrcv, int msg_id, int onoff);
extern "C" __declspec(dllimport) int OpenDevice_LXDeviceAPI(int LXDeviceID, ST_STREAMDATA_LXDAPI* pst_streamdata, int numsample_return, int mode);
extern "C" __declspec(dllimport) int SetMessageDevice_LXDeviceAPI(int device_handling_id, int msgtype_id, HWND hwnd_msgrcv, int msg_id, int onoff);
extern "C" __declspec(dllimport) int StartStream_LXDeviceAPI(int device_handling_id);
extern "C" __declspec(dllimport) int StopStream_LXDeviceAPI(int device_handling_id);
extern "C" __declspec(dllimport) int CloseDevice_LXDeviceAPI(int device_handling_id);
extern "C" __declspec(dllimport) int CloseApi_LXDeviceAPI(void);
extern "C" __declspec(dllimport) int GetStreamData_LXDeviceAPI(unsigned int message_wparam);
extern "C" __declspec(dllimport) int SetSampleFrequency_LXDeviceAPI(int device_handling_id, int sample_frequency);
extern "C" __declspec(dllimport) int GetSampleFrequency_LXDeviceAPI(int device_handling_id, int * sample_frequency);
extern "C" __declspec(dllimport) int GetFilterFrequency_LXDeviceAPI(int device_handling_id, int signal_source, float * freq_hpf, float * freq_lpf, float * freq_notch);
extern "C" __declspec(dllimport) int GetEEGRefElectrode_LXDeviceAPI(int device_handling_id, int * eeg_refelectrode);
extern "C" __declspec(dllimport) int SetDeviceControlPanel_LXDeviceAPI(int device_handling_id, int para0, int para1);
extern "C" __declspec(dllimport) int EventMarkingOnStream_LXDeviceAPI(int device_handling_id, unsigned int event_id); // from version 1.0.2.0
extern "C" __declspec(dllimport) int CheckForUpdate_LXDeviceAPI(int closeifnoupdate); // from version 1.0.3.0

#endif //#ifndef _LXDeviceAPI_H

