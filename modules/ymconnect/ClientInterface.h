
//
//
//This file contains the code needed to interface with foreign code (C#, Python, etc.)
//Data is passed to and from the foreign code using the functions and structures in this file.
//Foreign code<--->ClientInterface.h<--->LibInterface.h<--->C++ code



#pragma once

#include <cstdint>
#ifdef __cplusplus
extern "C" {
#endif
	//C types
	typedef char ICHAR8;
	typedef uint8_t IUINT8;
	typedef uint16_t UINT16;
	typedef uint32_t UINT32;

	typedef int8_t INT8;
	typedef int16_t INT16;
	typedef int32_t IINT32;

	typedef double DOUBLE64;
	typedef float FLOAT32;

	typedef struct ICoordinateArray coordinate_array_t;

	typedef struct IAxisConfigurationData axis_configuration_data_t;
	typedef struct IStatusInfo status_info_t;
	typedef struct IFigure* figure_t;
	typedef struct IPositionData position_data_t;
	typedef struct IXyzVector xyz_vector_t;
	typedef struct IEulerMatrix euler_matrix_t;
	typedef struct IPositionErrorData position_error_data_t;
	typedef struct ITorqueData torque_data_t;
	typedef struct IElapsedTime* elapsed_time_t;
	typedef struct ITimeData time_data_t;
	typedef struct ISystemInfoData system_info_data_t;
	typedef struct IControllerStateData controller_state_data_t;
	typedef struct IJobData job_data_t;
	typedef struct IActiveAlarms active_alarms_t;
	typedef struct IAlarmHistory alarm_history_t;
	typedef struct IByteVariableData byte_variable_data_t;
	typedef struct IIntegerVariableData integer_variable_data_t;
	typedef struct IDoubleIntVariableData double_variable_int_data_t;
	typedef struct IRealVariableData real_variable_data_t;
	typedef struct IStringVariableData string_variable_data_t;
	typedef struct IPositionVariableData position_variable_data_t;
	typedef struct IMotion motion_t;
	typedef struct IJobStack job_stack_t;
	

	typedef struct IMotomanController* motoman_controller_t;

	//FaultInterface
	YMCONNECT_API status_info_t fault_interface_get_active_alarms(motoman_controller_t controller, active_alarms_t& activeAlarmsData);
	YMCONNECT_API status_info_t fault_interface_get_alarm_history(motoman_controller_t controller, IINT32 alarmCategory, UINT32 quantity, alarm_history_t& alarmHistoryData);
	YMCONNECT_API status_info_t fault_interface_clear_all_faults(motoman_controller_t controller);
	
	//ControlGroupInterface
	YMCONNECT_API status_info_t control_group_interface_read_position_data(motoman_controller_t controller, IINT32 controlGroupId,
		IINT32 coordinateType, IUINT8 userFrame, IUINT8 toolNumber, position_data_t& positionData);
	YMCONNECT_API status_info_t control_group_interface_read_axis_configuration(motoman_controller_t controller, IINT32 controlGroupId,
		IAxisConfigurationData& axisConfiguration);
	YMCONNECT_API status_info_t control_group_interface_read_position_error(motoman_controller_t controller, IINT32 controlGroupId,
		position_error_data_t& positionErrorData);
	YMCONNECT_API status_info_t control_group_interface_read_torque_data(motoman_controller_t controller, IINT32 controlGroupId,
		torque_data_t& torqueData);
	
	//ByteVariableInterface
	YMCONNECT_API status_info_t byte_variable_interface_read(motoman_controller_t controller, UINT16 variableIndex, 
		byte_variable_data_t& byteVariableData);
	YMCONNECT_API status_info_t byte_variable_interface_write(motoman_controller_t controller, byte_variable_data_t byteVariableData);

	//IntegerVariableInterface
	YMCONNECT_API status_info_t integer_variable_interface_read(motoman_controller_t controller, UINT16 variableIndex,
		integer_variable_data_t& integerVariableData);
	YMCONNECT_API status_info_t integer_variable_interface_write(motoman_controller_t controller, integer_variable_data_t integerVariableData);

	//DoubleIntVariableInterface
	YMCONNECT_API status_info_t double_int_variable_interface_read(motoman_controller_t controller, UINT16 variableIndex,
		double_variable_int_data_t& doubleIntVariableData);
	YMCONNECT_API status_info_t double_int_variable_interface_write(motoman_controller_t controller, double_variable_int_data_t doubleIntVariableData);

	//RealVariableInterface
	YMCONNECT_API status_info_t real_variable_interface_read(motoman_controller_t controller, UINT16 variableIndex,
		real_variable_data_t& realVariableData);
	YMCONNECT_API status_info_t real_variable_interface_write(motoman_controller_t controller, real_variable_data_t realVariableData);

	//StringVariableInterface
	YMCONNECT_API status_info_t string_variable_interface_read(motoman_controller_t controller, UINT16 variableIndex,
		string_variable_data_t& stringVariableData);
	YMCONNECT_API status_info_t string_variable_interface_write(motoman_controller_t controller, const string_variable_data_t& stringVariableData);

	//RobotPositionVariableInterface
	YMCONNECT_API status_info_t robot_position_variable_interface_read(motoman_controller_t controller, UINT16 variableIndex,
		position_variable_data_t& positionVariableData);
	YMCONNECT_API status_info_t robot_position_variable_interface_write(motoman_controller_t controller, const position_variable_data_t& positionVariableData);

	//BasePositionVariableInterface
	YMCONNECT_API status_info_t base_position_variable_interface_read(motoman_controller_t controller, UINT16 variableIndex,
		position_variable_data_t& positionVariableData);
	YMCONNECT_API status_info_t base_position_variable_interface_write(motoman_controller_t controller, const position_variable_data_t& positionVariableData);

	//StationPositionVariableInterface
	YMCONNECT_API status_info_t station_position_variable_interface_read(motoman_controller_t controller, UINT16 variableIndex,
		position_variable_data_t& positionVariableData);
	YMCONNECT_API status_info_t station_position_variable_interface_write(motoman_controller_t controller, const position_variable_data_t& positionVariableData);

	//ControllerStateInterface
	YMCONNECT_API status_info_t controller_state_interface_read_state(motoman_controller_t controller, controller_state_data_t& controllerStateData);
	YMCONNECT_API status_info_t controller_state_interface_read_system_information(motoman_controller_t controller, IINT32 sysInfoId, system_info_data_t& systemInfoData);
	YMCONNECT_API status_info_t controller_state_interface_read_operating_times(motoman_controller_t controller, IINT32 id, IINT32 timeType, time_data_t& timeData, IINT32 appNumber);
	YMCONNECT_API status_info_t controller_state_read_system_param(motoman_controller_t controller, IINT32 parameterType, IINT32 parameterNumber, IINT32& parameterValue);
	YMCONNECT_API status_info_t controller_state_group_read_system_param(motoman_controller_t controller, IINT32 parameterType, IINT32 parameterNumber, 
		IINT32 parameterGroup, IINT32& parameterValue);

	//JobInterface
	YMCONNECT_API status_info_t job_interface_set_active_job(motoman_controller_t controller, const char* jobName, UINT32 lineNumber);
	YMCONNECT_API status_info_t job_interface_get_executing_job_information(motoman_controller_t controller, IINT32 selection, job_data_t& jobData);
	YMCONNECT_API status_info_t job_interface_get_job_stack(motoman_controller_t controller, IINT32 selection, IJobStack& jobNameArray);

	//FileInterface
	YMCONNECT_API status_info_t file_interface_LoadToControllerFromString(motoman_controller_t controller, const char* fileName, const char* contentsToLoad);
	YMCONNECT_API status_info_t file_interface_LoadToControllerFromPath(motoman_controller_t controller, const char* filePath);
	YMCONNECT_API status_info_t file_interface_SaveFromControllerToFile(motoman_controller_t controller, const char* fileName, const char* destinationFile, bool overwriteFlag = false);
	YMCONNECT_API ICHAR8* file_interface_SaveFromControllerToString(motoman_controller_t controller, const char* fileName, status_info_t& status);
	YMCONNECT_API status_info_t file_interface_DeleteJobFile(motoman_controller_t controller, const char* fileName);
	YMCONNECT_API status_info_t file_interface_GetFileCount(motoman_controller_t controller, IINT32 fileType, IINT32& fileCount);
	YMCONNECT_API ICHAR8* file_interface_ListFiles(motoman_controller_t controller, IINT32 fileType, status_info_t& status, bool sorted);

	//IOInterface
	YMCONNECT_API status_info_t io_interface_type_ReadBit(motoman_controller_t controller, IINT32 type, UINT16 group, IUINT8 bitPosition, IINT32& ioBit);
	YMCONNECT_API status_info_t io_interface_address_ReadBit(motoman_controller_t controller, UINT32 address, IINT32& ioBit);

	YMCONNECT_API status_info_t io_interface_type_ReadByte(motoman_controller_t controller, IINT32 type, UINT16 group, UINT32& io);
	YMCONNECT_API status_info_t io_interface_address_ReadByte(motoman_controller_t controller, UINT32 address, UINT32& io);
	YMCONNECT_API status_info_t io_interface_uint8_type_ReadByte(motoman_controller_t controller, IINT32 type, UINT16 group, IUINT8& io);
	YMCONNECT_API status_info_t io_interface_uint8_address_ReadByte(motoman_controller_t controller, UINT32 address, IUINT8& io);

	YMCONNECT_API status_info_t io_interface_type_ReadWord(motoman_controller_t controller, IINT32 type, UINT16 group, UINT32& io);
	YMCONNECT_API status_info_t io_interface_address_ReadWord(motoman_controller_t controller, UINT32 address, UINT32& io);
	YMCONNECT_API status_info_t io_interface_uint16_type_ReadWord(motoman_controller_t controller, IINT32 type, UINT16 group, UINT16& io);
	YMCONNECT_API status_info_t io_interface_uint16_address_ReadWord(motoman_controller_t controller, UINT32 address, UINT16& io);

	YMCONNECT_API status_info_t io_interface_address_WriteBit(motoman_controller_t controller, UINT32 address, IINT32 ioBit);
	YMCONNECT_API status_info_t io_interface_type_WriteBit(motoman_controller_t controller, IINT32 type, UINT16 group, IUINT8 bitPosition, IINT32 ioBit);

	YMCONNECT_API status_info_t io_interface_type_WriteByte(motoman_controller_t controller, IINT32 type, UINT16 group, IUINT8 io);
	YMCONNECT_API status_info_t io_interface_address_WriteByte(motoman_controller_t controller, UINT32 address, IUINT8 io);
	YMCONNECT_API status_info_t io_interface_uint8_type_WriteByte(motoman_controller_t controller, IINT32 type, UINT16 group, IUINT8 io);
	YMCONNECT_API status_info_t io_interface_uint8_address_WriteByte(motoman_controller_t controller, UINT32 address, IUINT8 io);

	YMCONNECT_API status_info_t io_interface_type_WriteWord(motoman_controller_t controller, IINT32 type, UINT16 group, UINT16 io);
	YMCONNECT_API status_info_t io_interface_address_WriteWord(motoman_controller_t controller, UINT32 address, UINT16 io);
	YMCONNECT_API status_info_t io_interface_uint16_type_WriteWord(motoman_controller_t controller, IINT32 type, UINT16 group, UINT16 io);
	YMCONNECT_API status_info_t io_interface_uint16_address_WriteWord(motoman_controller_t controller, UINT32 address, UINT16 io);

	YMCONNECT_API status_info_t io_interface_ReadRegister(motoman_controller_t controller, UINT16 registerAddress, UINT16& registerData);
	YMCONNECT_API status_info_t io_interface_WriteRegister(motoman_controller_t controller, UINT16 registerAddress, UINT16 registerData);
	YMCONNECT_API status_info_t io_interface_uint32_ReadRegister(motoman_controller_t controller, UINT16 registerAddress, UINT32& registerData);
	YMCONNECT_API status_info_t io_interface_uint32_WriteRegister(motoman_controller_t controller, UINT16 registerAddress, const UINT32& registerData);

	YMCONNECT_API UINT32 io_interface_ConvertIOGroupToBitAddress(motoman_controller_t controller, IINT32 type, UINT16 group, IUINT8 bitIndex, status_info_t& status_info_t);

	//ControlCommandsInterface
	YMCONNECT_API status_info_t control_commands_interface_SetServos(motoman_controller_t controller, bool signal);
	YMCONNECT_API status_info_t control_commands_interface_SetHold(motoman_controller_t controller, bool signal);
	YMCONNECT_API status_info_t control_commands_interface_SetCycleMode(motoman_controller_t controller, IINT32 cycleSelect);
	YMCONNECT_API status_info_t control_commands_interface_DisplayStringToPendant(motoman_controller_t controller, char* message);
	YMCONNECT_API status_info_t control_commands_interface_StartJob(motoman_controller_t controller);

	//MotionManagerInterface
	YMCONNECT_API status_info_t motion_manager_AddPointToTrajectory(motoman_controller_t controller, const motion_t& firstGroupTarget);
	YMCONNECT_API status_info_t motion_manager_Add2PointsToTrajectory(motoman_controller_t controller, const motion_t& firstGroupTarget, const motion_t& secondGroupTarget);
	YMCONNECT_API status_info_t motion_manager_Add3PointsToTrajectory(motoman_controller_t controller, const motion_t& firstGroupTarget, const motion_t& secondGroupTarget, const motion_t& thirdGroupTarget);
	YMCONNECT_API status_info_t motion_manager_Add4PointsToTrajectory(motoman_controller_t controller, const motion_t& firstGroupTarget, const motion_t& secondGroupTarget, const motion_t& thirdGroupTarget, const motion_t& fourthGroupTarget);
	YMCONNECT_API DOUBLE64 motion_manager_GetMotionTargetProgress(motoman_controller_t controller, IINT32 grp, status_info_t& statusCode);	
	YMCONNECT_API status_info_t motion_manager_MotionStart(motoman_controller_t controller);
	YMCONNECT_API status_info_t motion_manager_MotionStop(motoman_controller_t controller, bool clearTrajectory);
	YMCONNECT_API status_info_t motion_manager_ClearAllTrajectory(motoman_controller_t controller);

	//KinematicsInterface
	YMCONNECT_API status_info_t kinematics_interface_ConvertPosition(motoman_controller_t controller, IINT32 grp, position_data_t& positionToConvert, 
		IINT32 conversionType, position_data_t& convertedPosition);
	YMCONNECT_API status_info_t kinematics_interface_ConvertPositionFromCartesian(motoman_controller_t controller, IINT32 grp, position_data_t& positionToConvert,
		IINT32 conversionType, IINT32 type, const coordinate_array_t& prevAngle, position_data_t& convertedPosition);

	//VectorMath
	YMCONNECT_API void vector_math_inner_product(const xyz_vector_t& vector1, const xyz_vector_t& vector2, DOUBLE64& product);
	YMCONNECT_API void vector_math_cross_product(const xyz_vector_t& vector1, const xyz_vector_t& vector2, xyz_vector_t& product);

	//FrameMath
	YMCONNECT_API void frame_math_invert_frame(const euler_matrix_t& frame, euler_matrix_t& inverseFrame);
	YMCONNECT_API void frame_math_rotate_frame(const euler_matrix_t& org_frame, const xyz_vector_t& rotationVector, DOUBLE64 angle, euler_matrix_t& rotatedFrame);
	YMCONNECT_API void frame_math_frame_to_zyx_euler(const euler_matrix_t& frame, coordinate_array_t& coord);
	YMCONNECT_API void frame_math_multiply_frames(const euler_matrix_t& f1, const euler_matrix_t& f2, euler_matrix_t& productFrame);
	YMCONNECT_API void frame_math_zyx_euler_to_frame(const coordinate_array_t& coord, euler_matrix_t& frame);
	YMCONNECT_API void frame_math_set_identity_matrix_in_frame(euler_matrix_t& frame);

	//MotomanControllerInterface
	YMCONNECT_API motoman_controller_t open_connection(const char* ip_address, 
		status_info_t& status, 
		IINT32 receivePacketTimeout_ms, 
		IINT32 sendPacketRetries,
		bool loggingEnabled,
		const char* version);

	YMCONNECT_API void close_connection(motoman_controller_t controller);
	 
	//
#ifdef __cplusplus
}
#endif // __cplusplus

