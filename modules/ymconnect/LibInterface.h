
#pragma once

#include "YMConnect.h"
#include "InteropHelper.h"
#ifdef __cplusplus
extern "C" {
#endif

	struct IAxisConfigurationData
	{
		AxisConfigurationData _axisConfigurationData;
	};

	struct ICoordinateArray
	{
		CoordinateArray _coordinateArray;
	};

	struct IAlarmData
	{
		UINT32 code;
		UINT32 subcode;
		CHAR8* time;
		CHAR8* name;

		IAlarmData() = default;

		explicit IAlarmData(const AlarmData& alarmData)
		{
			code = alarmData.code;
			subcode = alarmData.subcode;
			time = InteropHelper::ConvertStringToChar8(alarmData.time);
			name = InteropHelper::ConvertStringToChar8(alarmData.name);
		}
	};
	struct IActiveAlarms
	{
		UINT32 numberOfActiveAlarms;
		std::array<IAlarmData, max_alarms> alarms;

		IActiveAlarms() = default;

		explicit IActiveAlarms(const ActiveAlarms& activeAlarmsData): numberOfActiveAlarms(activeAlarmsData.numberOfActiveAlarms)
		{
			for (UINT32 i = 0; i < max_alarms; i++)
			{
				alarms[i] = IAlarmData(activeAlarmsData.alarms[i]);
			}
		}				
	};

	struct IAlarmHistory
	{
		UINT32 numberOfAlarmsFetched;
		std::array<IAlarmData, alarm_history_size> alarms;

		IAlarmHistory() = default;

		explicit IAlarmHistory(const AlarmHistory& alarmHistoryData): numberOfAlarmsFetched(alarmHistoryData.numberOfAlarmsFetched)
		{
			for (UINT32 i = 0; i < alarm_history_size; i++)
			{
				alarms[i] = IAlarmData(alarmHistoryData.alarm[i]);
			}
		}
	};

	struct IPositionData
	{
		PositionData _positionData;
	};

	struct IPositionErrorData
	{
		PositionErrorData _positionError;
	};

	struct ITorqueData
	{
		TorqueData _torqueData;
	};

	struct IByteVariableData
	{
		ByteVariableData _byteVariableData;
	};

	struct IIntegerVariableData
	{
		IntegerVariableData _integerVariableData;
	};

	struct IDoubleIntVariableData
	{
				DoubleIntVariableData _doubleIntVariableData;
	};
	struct IRealVariableData
	{
		RealVariableData _realVariableData;
	};
	struct IStringVariableData
	{
		CHAR8* stringVariableData;
		UINT16 variableIndex;

		IStringVariableData() = default;

		explicit IStringVariableData(const StringVariableData& stringVariableData): stringVariableData(InteropHelper::ConvertStringToChar8(stringVariableData.stringVariableData)), variableIndex(stringVariableData.variableIndex)
		{
		}
				
	};
	struct IPositionVariableData
	{
		PositionData _positionVariableData;
		UINT16 variableIndex;

		IPositionVariableData() = default;

		IPositionVariableData(const RobotPositionVariableData& positionVariableData, UINT16 index): _positionVariableData(positionVariableData.positionData), variableIndex(index)
		{
		}

		IPositionVariableData(const BaseAxisPositionVariableData& positionVariableData, UINT16 index): _positionVariableData(positionVariableData.positionData), variableIndex(index)
		{
		}

		IPositionVariableData(const StationPositionVariableData& positionVariableData, UINT16 index): _positionVariableData(positionVariableData.positionData), variableIndex(index)
		{
		}

		explicit operator RobotPositionVariableData() const
		{
			RobotPositionVariableData positionVariableData;
			positionVariableData.positionData = _positionVariableData;
			positionVariableData.variableIndex = variableIndex;
			return positionVariableData;
		}

		explicit operator BaseAxisPositionVariableData() const
		{
			BaseAxisPositionVariableData positionVariableData;
			positionVariableData.positionData = _positionVariableData;
			positionVariableData.variableIndex = variableIndex;
			return positionVariableData;
		}

		explicit operator StationPositionVariableData() const
		{
			StationPositionVariableData positionVariableData;
			positionVariableData.positionData = _positionVariableData;
			positionVariableData.variableIndex = variableIndex;
			return positionVariableData;
		}		
	};

	struct IControllerStateData
	{
		ControllerStateData _controllerStateData;
	};

	struct ISystemInfoData
	{
		CHAR8* softwareVersion;
		CHAR8* modelName;

		ISystemInfoData() = default;

		explicit ISystemInfoData(const SystemInfoData& systemInfoData): softwareVersion(InteropHelper::ConvertStringToChar8(systemInfoData.softwareVersion)), modelName(InteropHelper::ConvertStringToChar8(systemInfoData.modelName))
		{
		}
	};

	struct ITimeData
	{
		TimeData _timeData;
	};

	struct IJobData
	{
		CHAR8* jobName;
		UINT32 line;
		UINT32 stepNumber;
		DOUBLE64 speedOverride;

		IJobData() = default;

		explicit IJobData(const JobData& jobData): jobName(InteropHelper::ConvertStringToChar8(jobData.name)), line(jobData.line), stepNumber(jobData.stepNumber), speedOverride(jobData.speedOverride)
		{
		}
	};

    struct IJobStack
	{
		std::array<CHAR8*, 12> JobNames{};

		IJobStack() = default;

		explicit IJobStack(const std::vector<std::string>& jobNames)
		{
			for (UINT32 i = 0; i < 12; i++)
			{
				if (i >= jobNames.size())
				{
					JobNames[i] = InteropHelper::ConvertStringToChar8("none");
					
				}
				else
				{
					JobNames[i] = InteropHelper::ConvertStringToChar8(jobNames[i].c_str());
				}
			
			}
		}
	};

	struct IMotion
	{
		Motion _motion;
	};

	struct IEulerMatrix
	{
		EulerMatrix _eulerMatrix;
	};

	struct IXyzVector
	{
		XyzVector _xyzVector;
	};

	struct IStatusInfo
	{
		IINT32 StatusCode{  };
		CHAR8* Message{ nullptr };

		IStatusInfo() = default;

		IStatusInfo(IINT32 statusCode, const CHAR8* message): StatusCode(statusCode), Message(InteropHelper::ConvertStringToChar8(message))
		{
		}

		explicit IStatusInfo(const StatusInfo& statusInfo): StatusCode(statusInfo.StatusCode), Message(InteropHelper::ConvertStringToChar8(statusInfo.Message))
		{
		}

		void SetStatusInfo(IINT32 statusCode, const char* message)
		{
			StatusCode = statusCode;
			Message = InteropHelper::ConvertStringToChar8(message);
		}
		void SetStatusInfo(const StatusInfo& statusInfo)
		{
			StatusCode = statusInfo.StatusCode;
			Message = InteropHelper::ConvertStringToChar8(statusInfo.Message);
		}
	};

	struct IMotomanController
	{
		MotomanController* _motomanController;

		IMotomanController() = default;

		explicit IMotomanController(MotomanController* motomanController): _motomanController(motomanController)
		{
		}
	};

#ifdef __cplusplus
}
#endif // __cplusplus



