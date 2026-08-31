/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "spi.h"
#include "tim.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* ═══════════════════════════════════════════
 *  协议常量
 * ═══════════════════════════════════════════ */
#define FRAME_LEN 5
#define CMD_WRITE_FLAG 0x80
#define CMD_ADDR_MASK 0x7F

/* ═══════════════════════════════════════════
 *  寄存器地址
 * ═══════════════════════════════════════════ */
#define REG_DEVICE_ID 0x00
#define REG_FW_VERSION 0x01
#define REG_STATUS 0x02
#define REG_CONTROL 0x03
#define REG_SAMPLE_RATE 0x04
#define REG_FIFO_LEVEL 0x05
#define REG_DATA_SEQ 0x06
#define REG_DATA_VAL 0x07
#define REG_OVERFLOW_COUNT 0x08
#define REG_SPI_REARM_FAIL 0x09
#define REG_SPI_ERROR_COUNT 0x0A
#define REG_NOP 0x7F

/* ═══════════════════════════════════════════
 *  STATUS 位定义
 * ═══════════════════════════════════════════ */
#define ST_RUNNING (1u << 0)
#define ST_CMD_ERR (1u << 1)
#define ST_RANGE_ERR (1u << 2)
#define ST_SPI_RESYNC (1u << 3)

/* ═══════════════════════════════════════════
 *  SPI 缓冲区
 * ═══════════════════════════════════════════ */
static uint8_t rx_buf[FRAME_LEN];
static uint8_t tx_buf[FRAME_LEN];

/* ═══════════════════════════════════════════
 *  寄存器状态
 * ═══════════════════════════════════════════ */
static volatile uint32_t reg_status = 0;
static volatile uint32_t reg_control = 0;
static volatile uint32_t reg_sample_rate = 1000;

/* ═══════════════════════════════════════════
 *  FIFO
 * ═══════════════════════════════════════════ */
#define FIFO_DEPTH 32

typedef struct
{
  uint32_t sequence;
  uint32_t value;
} FifoItem;

static FifoItem fifo_buf[FIFO_DEPTH];
static volatile uint16_t fifo_head = 0;     /* 下一个写入位置 */
static volatile uint16_t fifo_tail = 0;     /* 下一个读出位置 */
static volatile uint32_t fifo_overflow = 0; /* 溢出累计 */
static volatile uint32_t seq_counter = 0;   /* 全局递增序列号 */

/* 诊断计数器：HAL_SPI_TransmitReceive_IT 重新挂起失败次数，
 * 用来验证"锁死后从此再也接收不到新数据"这个假设是否成立 */
static volatile uint32_t spi_rearm_fail = 0;

/* 诊断计数器：HAL_SPI_ErrorCallback 总共被调用了几次。
 * 用来区分"CPU 还活着、SPI 硬件层面持续出错" 还是
 * "从某一刻起 CPU/中断整体停止响应"（这个数字会保持不再增长） */
static volatile uint32_t spi_error_count = 0;

/* peek/pop 中间状态 */
static uint32_t last_seq = 0;
static uint32_t last_val = 0;
static uint8_t item_peeked = 0;

/* ═══════════════════════════════════════════
 *  外设 handle（CubeMX 会在别处声明，这里 extern）
 * ═══════════════════════════════════════════ */
extern SPI_HandleTypeDef hspi2;
extern TIM_HandleTypeDef htim2;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

static uint32_t unpack_u32(const uint8_t *p);
static void pack_u32(uint8_t *p, uint32_t v);
static uint32_t reg_read(uint8_t addr);
static void reg_write(uint8_t addr, uint32_t val);
static void handle_frame(void);
static void spi_resync(void);
static void update_data_ready_gpio(void);
static void update_timer_rate(uint32_t rate_hz);
static void fifo_push(uint32_t value);
static uint8_t fifo_peek(uint32_t *seq_out, uint32_t *val_out);
static uint8_t fifo_pop(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ═══════════════════════════════════════════
 *  工具函数
 * ═══════════════════════════════════════════ */
static uint32_t unpack_u32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void pack_u32(uint8_t *p, uint32_t v)
{
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

/* ═══════════════════════════════════════════
 *  FIFO 操作
 * ═══════════════════════════════════════════ */
static uint16_t fifo_level(void) {
    uint16_t h = fifo_head;
    uint16_t t = fifo_tail;
    if (h >= t) return h - t;
    return FIFO_DEPTH - t + h;
}

static void fifo_push(uint32_t value) {
    uint16_t next = (fifo_head + 1) % FIFO_DEPTH;
    if (next == fifo_tail) {
        /* 满了，丢弃 */
        fifo_overflow++;
        return;
    }
    fifo_buf[fifo_head].sequence = seq_counter++;
    fifo_buf[fifo_head].value    = value;
    fifo_head = next;  /* 这一步让新数据对消费者可见 */
}

static uint8_t fifo_peek(uint32_t *seq_out, uint32_t *val_out) {
    if (fifo_head == fifo_tail) return 0;
    *seq_out = fifo_buf[fifo_tail].sequence;
    *val_out = fifo_buf[fifo_tail].value;
    return 1;
}

static uint8_t fifo_pop(void) {
    if (fifo_head == fifo_tail) return 0;
    fifo_tail = (fifo_tail + 1) % FIFO_DEPTH;
    return 1;
}
/* ═══════════════════════════════════════════
 *  DATA_READY GPIO
 * ═══════════════════════════════════════════ */
static void update_data_ready_gpio(void)
{
  // if (fifo_count > 0)
  // {
  //   HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
  // }
  // else
  // {
  //   HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
  // }

      if (fifo_head != fifo_tail) {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
    } else {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
    }

}

/* ═══════════════════════════════════════════
 *  定时器频率更新
 * ═══════════════════════════════════════════ */
static void update_timer_rate(uint32_t rate_hz)
{
  /*
   * 基频 = 72 MHz / 7200 = 10000 Hz
   * period = 10000 / rate_hz
   * rate_hz 合法范围 1–10000 → period 范围 1–10000
   */
  uint32_t period = 10000 / rate_hz;
  if (period < 1)
    period = 1;

  __HAL_TIM_SET_AUTORELOAD(&htim2, period - 1);
  __HAL_TIM_SET_COUNTER(&htim2, 0);
}

/* ═══════════════════════════════════════════
 *  SPI 错误恢复
 * ═══════════════════════════════════════════ */
static void spi_resync(void)
{
  volatile uint32_t tmp;

  __HAL_SPI_DISABLE(&hspi2);
  tmp = hspi2.Instance->DR;
  tmp = hspi2.Instance->SR;
  (void)tmp;
  __HAL_SPI_CLEAR_OVRFLAG(&hspi2);

  hspi2.State = HAL_SPI_STATE_READY;
  hspi2.ErrorCode = HAL_SPI_ERROR_NONE;
  hspi2.Lock = HAL_UNLOCKED; /* 防止上次错误路径没释放锁，导致之后所有
                                HAL_SPI_TransmitReceive_IT 调用永久返回
                                HAL_BUSY 而悄悄失效 */

  reg_status |= ST_SPI_RESYNC;

  __HAL_SPI_ENABLE(&hspi2);
}

/* ═══════════════════════════════════════════
 *  寄存器读
 * ═══════════════════════════════════════════ */
static uint32_t reg_read(uint8_t addr)
{
  switch (addr)
  {
  /* ── V1.2 原有 ── */
  case REG_DEVICE_ID:
    return 0xAC00ACC0u;

  case REG_FW_VERSION:
    return 0x00010300u; /* V1.3 → 0x00010300 */

  case REG_STATUS:
    return reg_status;

  case REG_CONTROL:
    return reg_control;

  case REG_SAMPLE_RATE:
    return reg_sample_rate;

  case REG_NOP:
    return 0;

  /* ── V1.3 新增 ── */
  case REG_FIFO_LEVEL:
    return fifo_level();

  case REG_DATA_SEQ:
    if (fifo_peek(&last_seq, &last_val))
    {
      item_peeked = 1;
      return last_seq;
    }
    return 0;

  case REG_DATA_VAL:
    if (item_peeked)
    {
      item_peeked = 0;
      fifo_pop();
      update_data_ready_gpio();
      return last_val;
    }
    if (fifo_peek(&last_seq, &last_val))
    {
      fifo_pop();
      update_data_ready_gpio();
      return last_val;
    }
    return 0;

  case REG_OVERFLOW_COUNT:
    return fifo_overflow;

  case REG_SPI_REARM_FAIL:
    return spi_rearm_fail;

  case REG_SPI_ERROR_COUNT:
    return spi_error_count;

  default:
    reg_status |= ST_CMD_ERR;
    return 0;
  }
}

/* ═══════════════════════════════════════════
 *  寄存器写
 * ═══════════════════════════════════════════ */
static void reg_write(uint8_t addr, uint32_t val)
{
  switch (addr)
  {
  case REG_CONTROL:
    /* CLEAR_FLAGS */
    if (val & 0x02u)
    {
      reg_status &= ~(ST_CMD_ERR | ST_RANGE_ERR | ST_SPI_RESYNC);
      val &= ~0x02u;
    }
    reg_control = val & 0x01u;

    if (reg_control & 0x01u)
    {
      /* START */
      reg_status |= ST_RUNNING;
      seq_counter = 0;
      fifo_head = 0;
      fifo_tail = 0;
      fifo_overflow = 0;
      item_peeked = 0;
      update_data_ready_gpio();
      update_timer_rate(reg_sample_rate);
      HAL_TIM_Base_Start_IT(&htim2);
    }
    else
    {
      /* STOP */
      reg_status &= ~ST_RUNNING;
      HAL_TIM_Base_Stop_IT(&htim2);
      update_data_ready_gpio();
    }
    break;

  case REG_SAMPLE_RATE:
    if (val >= 1u && val <= 10000u)
    {
      reg_sample_rate = val;
      /* 如果正在采集，立刻更新频率 */
      if (reg_control & 0x01u)
      {
        update_timer_rate(val);
      }
    }
    else
    {
      reg_status |= ST_RANGE_ERR;
    }
    break;

  default:
    /* 只读寄存器或未定义地址 */
    reg_status |= ST_CMD_ERR;
    break;
  }
}

/* ═══════════════════════════════════════════
 *  帧处理（SPI 回调里调用）
 * ═══════════════════════════════════════════ */
static void handle_frame(void)
{
  uint8_t cmd = rx_buf[0];
  uint8_t addr = cmd & CMD_ADDR_MASK;
  uint32_t resp = 0;

  if (cmd & CMD_WRITE_FLAG)
  {
    reg_write(addr, unpack_u32(&rx_buf[1]));
    resp = 0;
  }
  else
  {
    resp = reg_read(addr);
  }

  tx_buf[0] = cmd; /* ECHO */
  pack_u32(&tx_buf[1], resp);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_SPI2_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */

  /* 准备第一帧 tx_buf（上电后第一帧 slave 回的是这个） */
  tx_buf[0] = REG_NOP;
  pack_u32(&tx_buf[1], 0);

  /* DATA_READY 初始拉低 */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);

  /* 启动 SPI 中断接收 —— 调用后立刻返回，不阻塞 */
  HAL_SPI_TransmitReceive_IT(&hspi2, tx_buf, rx_buf, FRAME_LEN);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* ═══════════════════════════════════════════
 *  SPI 中断回调：一帧收发完成
 * ═══════════════════════════════════════════ */
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
  if (hspi->Instance == SPI2)
  {
    handle_frame();
    /* 立刻准备接收下一帧 */
    if (HAL_SPI_TransmitReceive_IT(&hspi2, tx_buf, rx_buf, FRAME_LEN) != HAL_OK)
    {
      spi_rearm_fail++;
    }
  }
}

/* ═══════════════════════════════════════════
 *  SPI 错误回调
 * ═══════════════════════════════════════════ */
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
  if (hspi->Instance == SPI2)
  {
    spi_error_count++;
    spi_resync();
    if (HAL_SPI_TransmitReceive_IT(&hspi2, tx_buf, rx_buf, FRAME_LEN) != HAL_OK)
    {
      spi_rearm_fail++;
    }
  }
}

/* ═══════════════════════════════════════════
 *  定时器中断回调：产生一个 sample
 * ═══════════════════════════════════════════ */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM2)
  {
    if (reg_control & 0x01u)
    {
      /*
       * 合成数据：用序列号的低 16 位作为值
       * 你也可以换成正弦查表、三角波等任何合成波形
       */
      uint32_t sample_value = seq_counter & 0xFFFFu;
      fifo_push(sample_value);
      update_data_ready_gpio();
    }
  }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
