
#include "app_can.h"
#include "sys_config.h"
#include "msg_pubsub.h"
#include "msg_topics.h"

#include "act_state.h"

#if SYS_ENABLE_UDS
#include "uds/uds_rx_entry.h"
#include "uds/uds_ota.h"
#include "uds/adapter_can.h"
#endif

//canģ��ʹ��
#if SYS_ENABLE_CAN

/* ����״̬������ */
typedef struct {
    uint32_t last_cycle;        // ��һ���������ڵ���ʼʱ��
    uint32_t next_tx_time;      // ��һ֡�ķ���ʱ���
    uint8_t  step;              // 0:����, 1~7:���͵ڼ�֡

    /* �Ƹ�ʹ�ܱ�־ */
    uint8_t motor1_enabled;   // �Ƹ�1ʹ�ܣ�1=ʹ�ܣ�0=����
    uint8_t motor2_enabled;   // �Ƹ�2ʹ�ܣ�1=ʹ�ܣ�0=����

	uint8_t software_enable;

} can_tx_manager_t;

static can_tx_manager_t tx_mgr = {0};

/* �ⲿӲ�����ñ� */
extern const can_cfg_t CAN_HW;

/* ȫ��ʵ������̬�������ļ��ɼ��� */
can_handle_t can_handle;
// �����������״̬ʵ��
motor_state_t g_motor_state[2];
//׼��������
static can_msg_t s_can_msg = {
    .motor1_cmd          = CAN_CMD_IDLE,
    .motor2_cmd          = CAN_CMD_IDLE,
    .highest_gear_signal = 0,
    .en_gk_only          = SPC_DISABLE,                     // ���� machine_type_spc_t ������0
    .support_calibration = CALIB_NOT_SUPPORT,
    .awd_cal_enable      = AWD_CAL_DISABLE,
    .speed_general       = 0,
    .speed_shengshuo     = 0,
    .gear_status         = GEAR_INVALID,
    .machine_type        = 0,
    .ce4p_spc            = SPC_DISABLE,
};

/* ==================== 5. ��ʱ���ͺ���������ѭ�������Ե��ã� ==================== */
// ���ͼ�������룩������ʵ��������������� 500ms��
#define CAN_TX_INTERVAL_MS 500

/* ==================== �޸ĺ�����ݴ������� ==================== */
void data_process_9DF0(can_rx_data_t *data, uint8_t len) {
    if (len < 8) return;

    // ���µ��ָ��
    s_can_msg.motor1_cmd = data->id_9DF0.byte0.cmd_m1;
    s_can_msg.motor2_cmd = data->id_9DF0.byte0.cmd_m2;

    // ������ߵ��ź�
    s_can_msg.highest_gear_signal = data->id_9DF0.byte1.highest_gear_signal;

    // ���� en_gk_only �� support_calibration
    s_can_msg.en_gk_only = data->id_9DF0.byte2.en_gk_only;
    s_can_msg.support_calibration = data->id_9DF0.byte2.support_calibration;

    // �����ֶΣ�����Ҫ�����ӣ�
}

void data_process_8018(can_rx_data_t *data, uint8_t len) {
    if (len < 8) return;
    // ���� support_calibration����һ��Դ��
    s_can_msg.support_calibration = data->id_8018.support_calibration;
}

void data_process_EC18(can_rx_data_t *data, uint8_t len) {
    if (len < 8) return;
    s_can_msg.awd_cal_enable = data->id_EC18.awd_cal_enable;
}

void data_process_17F0(can_rx_data_t *data, uint8_t len) {
    if (len < 8) return;
    s_can_msg.gear_status = data->id_17F0.byte2.gear_status;
}

void data_process_FDF0(can_rx_data_t *data, uint8_t len) {
    if (len < 8) return;
    uint16_t speed_raw = ((uint16_t)data->id_FDF0.speed_high << 8) | data->id_FDF0.speed_low;
    s_can_msg.speed_general = speed_raw;  // �洢ԭʼֵ (0.1km/h)
}

void data_process_31F9(can_rx_data_t *data, uint8_t len) {
    if (len < 8) return;
    uint16_t speed_raw = ((uint16_t)data->id_31F9.speed_high << 8) | data->id_31F9.speed_low;
    s_can_msg.speed_shengshuo = speed_raw;
}

void data_process_8818(can_rx_data_t *data, uint8_t len) {
    if (len < 8) return;
    s_can_msg.machine_type = data->id_8818.machine_type;
    s_can_msg.ce4p_spc = data->id_8818.byte3.ce4p_spc;
}

//״̬����
static void can_publish_stat(void)
{
	Msg_Publish(TOPIC_CAN_MSG_RECEIVED, &s_can_msg, sizeof(can_msg_t), MSG_PRIO_HIGH);
}

/* ==================== ���մ������������޸ĺ� ==================== */
void app_can_receive(void) {
    stc_can_rx_frame_t rxFrame;
    if (can_read(&can_handle.can_rx, &rxFrame) == CAN_RET_OK) {
        can_rx_message_t rx_mess;
        memset(&rx_mess, 0, sizeof(rx_mess));

        rx_mess.can_id = rxFrame.u32ID;
        rx_mess.len = rxFrame.DLC;
        memcpy(&rx_mess.data, rxFrame.au8Data, rx_mess.len);

        switch (rx_mess.can_id) {
            case 0x18FF9DF0:
            case 0x18FFC060:
                data_process_9DF0(&rx_mess.data, rx_mess.len);
                break;
            case 0x18FF8018:
                data_process_8018(&rx_mess.data, rx_mess.len);
                break;
            case 0x18FFEC18:
                data_process_EC18(&rx_mess.data, rx_mess.len);
                break;
            case 0x18FF17F0:
                data_process_17F0(&rx_mess.data, rx_mess.len);
                break;
            case 0x18FFFDF0:
                data_process_FDF0(&rx_mess.data, rx_mess.len);
                break;
            case 0x18FF31F9:
                data_process_31F9(&rx_mess.data, rx_mess.len);
                break;
            case 0x18FF8818:
            case 0x18FFFFF0:
                data_process_8818(&rx_mess.data, rx_mess.len);
                break;
            default:
#if SYS_ENABLE_UDS
                uds_rx_entry(rx_mess.can_id, rx_mess.data.rx_data, rx_mess.len);
#endif
                return;  // δ֪ ID��������
        }
        // ÿ�γɹ������󷢲�����״̬
        can_publish_stat();
    }
}
/**
 * @brief ���� ID 0x18FF0108 �������ݣ����1��
 * @param data ָ�� can_tx_data_t ��ָ�루������䣩
 * @param motor ���״ָ̬��
 */
static void build_can_0108(can_tx_data_t *data, const motor_state_t *motor)
{
    // ��� byte0
    data->id_0108.byte0.cal_stat = motor->cal_stat;
    data->id_0108.byte0.run_stat = motor->run_stat;
    data->id_0108.byte0.resv0 = 0;

    // ��������ѹ�����ֽ���ǰ��
    data->id_0108.sensor_vol_l = (uint8_t)(motor->sensor_voltage & 0xFF);
    data->id_0108.sensor_vol_h = (uint8_t)(motor->sensor_voltage >> 8);

    // ���������ֽ���ǰ��
    data->id_0108.current_l = (uint8_t)(motor->current & 0xFF);
    data->id_0108.current_h = (uint8_t)(motor->current >> 8);

    // �����ֽ�
    data->id_0108.resv1[0] = 0;
    data->id_0108.resv1[1] = 0;

    // ����״̬
    data->id_0108.fault_stat = motor->fault_stat;
}

/**
 * @brief ���� ID 0x18FF0104 �������ݣ����2��
 * @param data ָ�� can_tx_data_t ��ָ��
 * @param motor ���״ָ̬��
 */
static void build_can_0104(can_tx_data_t *data, const motor_state_t *motor)
{
    // ��� byte0����0108��ͬ��
    data->id_0104.byte0.cal_stat = motor->cal_stat;
    data->id_0104.byte0.run_stat = motor->run_stat;
    data->id_0104.byte0.resv0 = 0;

    // ����2�ֽ�
    data->id_0104.resv1[0] = 0;
    data->id_0104.resv1[1] = 0;

    // ���������ֽ���ǰ��
    data->id_0104.current_l = (uint8_t)(motor->current & 0xFF);
    data->id_0104.current_h = (uint8_t)(motor->current >> 8);

    // ��������ѹ�����ֽ���ǰ��
    data->id_0104.sensor_vol_l = (uint8_t)(motor->sensor_voltage & 0xFF);
    data->id_0104.sensor_vol_h = (uint8_t)(motor->sensor_voltage >> 8);

    // ����״̬
    data->id_0104.fault_stat = motor->fault_stat;
}

static void build_can_010A(can_tx_data_t *data, const motor_state_t *motor)
{
    memset(data, 0, sizeof(can_tx_data_t));

    // byte0: �궨/����״̬
    data->id_010A.byte0.cal_stat = motor->cal_stat;
    data->id_010A.byte0.run_stat = motor->run_stat;
    data->id_010A.byte0.resv0 = 0;

    // byte1~2: �Ƹ��г�, 0.1mm, С��
    data->id_010A.pos_l = (uint8_t)(motor->position & 0xFF);
    data->id_010A.pos_h = (uint8_t)(motor->position >> 8);

    // byte3: ����״̬ (byte4~7 ����, memset ����0)
    data->id_010A.fault_stat = motor->fault_stat;
}

static void build_can_1108(can_tx_data_t *data)
{
	memset(data, 0, sizeof(can_tx_data_t));

	data->tx_data[0] = 0;
	data->tx_data[1] = 0;
	data->tx_data[2] = 0;
	data->tx_data[3] = 4;
}

/**
 * @brief ���� ID 0x18FF1108 �������� ���������汾��
 * @param data ָ�� can_tx_data_t ��ָ��
 * @param motor ���״ָ̬��
 */

/* ==================== 4. ��ȡ���״̬��Ϣ�Ľӿڣ�Ԥ���� ==================== */
/**
 * @brief ���µ��1��״̬����Ӳ�����㷨�л�ȡ��
 * �˺���Ӧ����ѭ����ʱ���е��ã�����ˢ�� g_motor1_state
 */
static void update_motor_state(uint8_t motor_idx)
{

	//�궨״̬
	if (true == mySystem.axis[motor_idx].is_calib)
	{
		g_motor_state[motor_idx].cal_stat = CAL_ENABLE;
	}else
	{
		g_motor_state[motor_idx].cal_stat = CAL_DISABLE;
	}

	//����״̬
	if (ACT_MODE_CALIB_MODE == mySystem.axis[motor_idx].mode)
	{
		if (CALIB_STATE_MANU_OUT == mySystem.axis[motor_idx].sm_calib.cur_state)
		{
			if (mySystem.axis[motor_idx].dir == ACT_DIR_COMBINE_IS_MOVE_OUT)
			{
				g_motor_state[motor_idx].run_stat = RUN_STAT_MANU_COMB;
				g_motor_state[motor_idx].fault_stat = FAULT_STAT_IDLE;
			}else if (mySystem.axis[motor_idx].dir == ACT_DIR_COMBINE_IS_MOVE_IN)
			{
				g_motor_state[motor_idx].run_stat = RUN_STAT_MANU_SEP;
				g_motor_state[motor_idx].fault_stat = FAULT_STAT_IDLE;
			}
		}
		else if (CALIB_STATE_MANU_IN== mySystem.axis[motor_idx].sm_calib.cur_state)
		{
			if (mySystem.axis[motor_idx].dir == ACT_DIR_COMBINE_IS_MOVE_OUT)
			{
				g_motor_state[motor_idx].run_stat = RUN_STAT_MANU_SEP;
				g_motor_state[motor_idx].fault_stat = FAULT_STAT_IDLE;
			}else if (mySystem.axis[motor_idx].dir == ACT_DIR_COMBINE_IS_MOVE_IN)
			{
				g_motor_state[motor_idx].run_stat = RUN_STAT_MANU_COMB;
				g_motor_state[motor_idx].fault_stat = FAULT_STAT_IDLE;
			}
		}
		//һ���Զ��궨ֱ���ϱ��ѱ궨�ѷ���
		else if (CALIB_STATE_AUTO_CALIB_SEP == mySystem.axis[motor_idx].sm_calib.cur_state ||
			CALIB_STATE_AUTO_CALIB_BACK == mySystem.axis[motor_idx].sm_calib.cur_state)
		{
			g_motor_state[motor_idx].cal_stat = CAL_ENABLE;
			g_motor_state[motor_idx].run_stat = RUN_STAT_SEP_DONE;
			g_motor_state[motor_idx].fault_stat = FAULT_STAT_IDLE;
		}else
		{
			g_motor_state[motor_idx].run_stat = RUN_STAT_IDLE;
			g_motor_state[motor_idx].fault_stat = FAULT_STAT_IDLE;
		}

	}else if (ACT_MODE_NORMAL == mySystem.axis[motor_idx].mode || ACT_MODE_IDLE == mySystem.axis[motor_idx].mode)
	{
		if (mySystem.axis[motor_idx].pos_err == POS_NO)
		{
			g_motor_state[motor_idx].run_stat = RUN_STAT_IDLE;
			g_motor_state[motor_idx].fault_stat = FAULT_STAT_IDLE;
		}
		else
		{
			if (false == mySystem.axis[motor_idx].is_calib)
			{
				g_motor_state[motor_idx].run_stat = RUN_STAT_IDLE;
				g_motor_state[motor_idx].fault_stat = FAULT_STAT_IDLE;
			}else
			{
				if (ACT_STATE_COMBINE_FAIL == mySystem.axis[motor_idx].sm_act.cur_state)//�����쳣��״̬
				{
					EventGroup_Send(mySystem.axis[motor_idx].evt_act, EVT_ACT_RESET);
					g_motor_state[motor_idx].run_stat = RUN_STAT_ERR_COMB;
					g_motor_state[motor_idx].fault_stat = FAULT_STAT_ERR_COMB;
				}
				else if (ACT_STATE_COMBINE_BACK_2 == mySystem.axis[motor_idx].sm_act.cur_state)//�����쳣��״̬
				{
					g_motor_state[motor_idx].run_stat = RUN_STAT_ERR_COMB;
					g_motor_state[motor_idx].fault_stat = FAULT_STAT_ERR_COMB;
				}
				else if (ACT_STATE_SEPARATE_FAIL == mySystem.axis[motor_idx].sm_act.cur_state)//
				{
					EventGroup_Send(mySystem.axis[motor_idx].evt_act, EVT_ACT_RESET);
					g_motor_state[motor_idx].run_stat = RUN_STAT_ERR_SEP;
					g_motor_state[motor_idx].fault_stat = FAULT_STAT_ERR_SEP;
				}
				else if (ACT_STATE_SEPARATE_BACK_2 == mySystem.axis[motor_idx].sm_act.cur_state)//
				{
					g_motor_state[motor_idx].run_stat = RUN_STAT_ERR_SEP;
					g_motor_state[motor_idx].fault_stat = FAULT_STAT_ERR_SEP;
				}
				else if (ACT_STATE_OVERSPEED_ALARM == mySystem.axis[motor_idx].sm_act.cur_state)//
				{
					g_motor_state[motor_idx].run_stat = RUN_STAT_ERR_SPD;
					g_motor_state[motor_idx].fault_stat = FAULT_STAT_ERR_SPD;
				}
				else if (ACT_STATE_H_GEAR_COMB_5S == mySystem.axis[motor_idx].sm_act.cur_state)//
				{
					g_motor_state[motor_idx].run_stat = RUN_STAT_ERR_GEAR_STOP;
					g_motor_state[motor_idx].fault_stat = FAULT_STAT_ERR_GEAR_STOP;
				}
				else if (ACT_STATE_H_GEAR_COMB_ED == mySystem.axis[motor_idx].sm_act.cur_state)//
				{
					g_motor_state[motor_idx].run_stat = RUN_STAT_ERR_COMB_GEAR;
					g_motor_state[motor_idx].fault_stat = FAULT_STAT_ERR_COMB_GEAR;
				}
				else if (ACT_STATE_COMBINE_SUCCESS == mySystem.axis[motor_idx].sm_act.cur_state)//��ϳɹ�
				{
					EventGroup_Send(mySystem.axis[motor_idx].evt_act, EVT_ACT_RESET);
					g_motor_state[motor_idx].run_stat = RUN_STAT_COMB_DONE;
					g_motor_state[motor_idx].fault_stat = FAULT_STAT_IDLE;
				}
				else if (ACT_STATE_SEPARATE_SUCCESS == mySystem.axis[motor_idx].sm_act.cur_state)//����ɹ�
				{
					EventGroup_Send(mySystem.axis[motor_idx].evt_act, EVT_ACT_RESET);
					g_motor_state[motor_idx].run_stat = RUN_STAT_SEP_DONE;
					g_motor_state[motor_idx].fault_stat = FAULT_STAT_IDLE;
				}
				else if (ACT_STATE_COMBINE == mySystem.axis[motor_idx].sm_act.cur_state ||
					ACT_STATE_COMBINE_TRY_1 == mySystem.axis[motor_idx].sm_act.cur_state ||
					ACT_STATE_COMBINE_TRY_2 == mySystem.axis[motor_idx].sm_act.cur_state ||
					ACT_STATE_COMBINE_BACK == mySystem.axis[motor_idx].sm_act.cur_state ||
					ACT_STATE_COMBINE_BACK_1 == mySystem.axis[motor_idx].sm_act.cur_state)//����ɹ�
				{
					g_motor_state[motor_idx].run_stat = RUN_STAT_AUTO_COMB;
					g_motor_state[motor_idx].fault_stat = FAULT_STAT_IDLE;
				}
				else if (ACT_STATE_SEPARATE == mySystem.axis[motor_idx].sm_act.cur_state ||
					ACT_STATE_SEPARATE_TRY_1 == mySystem.axis[motor_idx].sm_act.cur_state ||
					ACT_STATE_SEPARATE_TRY_2 == mySystem.axis[motor_idx].sm_act.cur_state ||
					ACT_STATE_SEPARATE_BACK == mySystem.axis[motor_idx].sm_act.cur_state ||
					ACT_STATE_SEPARATE_BACK_1 == mySystem.axis[motor_idx].sm_act.cur_state)//����ɹ�
				{
					g_motor_state[motor_idx].run_stat = RUN_STAT_AUTO_SEP;
					g_motor_state[motor_idx].fault_stat = FAULT_STAT_IDLE;
				}
				else if (ACT_STATE_IDLE == mySystem.axis[motor_idx].sm_act.cur_state ||
					ACT_STATE_OVERSPEED == mySystem.axis[motor_idx].sm_act.cur_state ||
					ACT_STATE_H_GEAR_NO_COMB == mySystem.axis[motor_idx].sm_act.cur_state)
				{
					if (ACT_STATE_COMBINE_FAIL == mySystem.axis[motor_idx].sm_act.previous_state || ACT_STATE_SEPARATE_FAIL == mySystem.axis[motor_idx].sm_act.previous_state)//��һ��״̬�ǽ��/����ʧ�ܣ�״̬���ܱ�
					{
						//
					}else
					{
						if (ACT_POS_COMBINE == mySystem.axis[motor_idx].position)
						{
							g_motor_state[motor_idx].run_stat = RUN_STAT_COMB_DONE;
							g_motor_state[motor_idx].fault_stat = FAULT_STAT_IDLE;
						}else if (ACT_POS_SEPARATE == mySystem.axis[motor_idx].position)
						{
							g_motor_state[motor_idx].run_stat = RUN_STAT_SEP_DONE;
							g_motor_state[motor_idx].fault_stat = FAULT_STAT_IDLE;
						}else
						{
							g_motor_state[motor_idx].run_stat = RUN_STAT_IDLE;
							g_motor_state[motor_idx].fault_stat = FAULT_STAT_IDLE;
						}
					}

				}
			}
		}
	}

    g_motor_state[motor_idx].sensor_voltage = mySystem.axis[motor_idx].pos_vol;    // ���� 25.0V
    g_motor_state[motor_idx].current =  mySystem.axis[motor_idx].current;

    // �Ƹ��г�: 0.1mm; ��λ���쳣(����/λ�ô���)ʱ�� 0xFFFF
    if (POS_NO == mySystem.axis[motor_idx].pos_err || POS_ERR == mySystem.axis[motor_idx].pos_err)
    {
        g_motor_state[motor_idx].position = 0xFFFF;
    }
    else
    {
        g_motor_state[motor_idx].position = (uint16_t)(mySystem.axis[motor_idx].pos_current * 10.0f);
    }
}

void app_can_set_tx_mgr(uint8_t motor_idx, uint8_t enable) //0 - ʧ�ܣ� 1 - ʹ��
{
	if (0 == motor_idx)
	{
		tx_mgr.motor1_enabled = enable;
	}
	else if (1 == motor_idx)
	{
		tx_mgr.motor2_enabled = enable;
	}

}

/* Ӧ�ò㷢�ͺ�������Ҫ����Ƶ���ã�����ÿ1ms����̣� */
void app_can_transmit(void)
{
    uint32_t now = SysTick_GetTick();

	/* ˢ�µ��״̬����ȡ�������ݣ� */
    update_motor_state(0);
    update_motor_state(1);

    /* ״̬0�����У�����Ƿ��������ڣ�500ms�� */
    if (tx_mgr.step == 0) {
        if (now - tx_mgr.last_cycle >= 500) {
            tx_mgr.last_cycle = now;
            tx_mgr.step = 1;                // ��ʼ���͵�һ֡
            tx_mgr.next_tx_time = now;      // ��������
        } else {
            return;
        }
    }

    /* �ȴ�����ʱ�� */
    if (now < tx_mgr.next_tx_time) {
        return;
    }

    can_tx_data_t tx_data;

    /* �����跢�� */
    switch (tx_mgr.step) {
        case 1:
			if (tx_mgr.motor1_enabled)
			{
                build_can_0108(&tx_data, &g_motor_state[0]);
                can_transmit_ext(0x18FF0108, (uint8_t*)&tx_data, 8);
			}
            break;
        case 2:
			if (tx_mgr.motor2_enabled)
			{
                build_can_0108(&tx_data, &g_motor_state[1]);
                can_transmit_ext(0x18FF0109, (uint8_t*)&tx_data, 8);
			}
            break;
        case 3:
			if (tx_mgr.motor1_enabled)
			{
                build_can_0104(&tx_data, &g_motor_state[0]);
                can_transmit_ext(0x18FF0104, (uint8_t*)&tx_data, 8);
			}
            break;
        case 4:
			if (tx_mgr.motor2_enabled)
			{
                build_can_0104(&tx_data, &g_motor_state[1]);
                can_transmit_ext(0x18FF0105, (uint8_t*)&tx_data, 8);
			}
            break;
        case 5:
			if (tx_mgr.motor1_enabled || tx_mgr.motor2_enabled)
			{
				build_can_1108(&tx_data);
				can_transmit_ext(0x18FF1108, (uint8_t*)&tx_data, 8);
			}

            break;
        case 6:
            if (tx_mgr.motor1_enabled)
            {
                build_can_010A(&tx_data, &g_motor_state[0]);
                can_transmit_ext(0x18FF010A, (uint8_t*)&tx_data, 8);
            }
            break;
        case 7:
            if (tx_mgr.motor2_enabled)
            {
                build_can_010A(&tx_data, &g_motor_state[1]);
                can_transmit_ext(0x18FF010B, (uint8_t*)&tx_data, 8);
            }
            break;
        default:
            break;
    }

    /* �ƶ�����һ֡ */
    tx_mgr.step++;
    if (tx_mgr.step > 7) {
        tx_mgr.step = 0;                // һ����ɣ��ص�����
        tx_mgr.next_tx_time = 0;
    } else {
        tx_mgr.next_tx_time = now + 1;  // ���1ms����������
    }
}

//������ı궨��־
void can_clr_awd_cal(void)
{
	s_can_msg.awd_cal_enable = AWD_CAL_DISABLE;
}

//�����������
void can_clr_cmd(uint8_t motor_idx)
{
	if (0 == motor_idx)
	{
		s_can_msg.motor1_cmd = CAN_CMD_IDLE;
	}else if (1 == motor_idx)
	{
		s_can_msg.motor2_cmd = CAN_CMD_IDLE;
	}

}

//��ʼ��
void app_can_init(void)
{
	can_module_init(&can_handle,&CAN_HW);
#if SYS_ENABLE_UDS
	CanIf_Init(&can_handle);
	UdsOta_Init();
#endif

	//����������Ϣ�Ա��Ľ���ʹ��

}

//���ڵ��Ⱥ���
void app_can_task(void)
{
	//can����
	app_can_receive();
	//can����
	app_can_transmit();
	// //״̬����app_can_receive���յ������ٷ���
	// can_evt_publish(&s_can_msg);
}

#endif
