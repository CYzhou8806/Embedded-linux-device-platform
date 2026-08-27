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

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

#define FRAME_LEN        5
#define CMD_WRITE_FLAG   0x80
#define CMD_ADDR_MASK    0x7F

#define REG_DEVICE_ID    0x00
#define REG_FW_VERSION   0x01
#define REG_STATUS       0x02
#define REG_CONTROL      0x03
#define REG_SAMPLE_RATE  0x04
#define REG_NOP          0x7F

#define ST_RUNNING       (1u << 0)
#define ST_CMD_ERR       (1u << 1)
#define ST_RANGE_ERR     (1u << 2)
#define ST_SPI_RESYNC    (1u << 3)

static uint8_t  rx_buf[FRAME_LEN];
static uint8_t  tx_buf[FRAME_LEN];

static uint32_t reg_status      = 0;
static uint32_t reg_control     = 0;
static uint32_t reg_sample_rate = 1000;

uint32_t unpack_u32(const uint8_t *p);
void pack_u32(uint8_t *p, uint32_t v);
uint32_t reg_read(uint8_t addr);
void reg_write(uint8_t addr, uint32_t val);
void handle_frame(void);
void spi_resync(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

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
  /* USER CODE BEGIN 2 */

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
	tx_buf[0] = REG_NOP;
	pack_u32(&tx_buf[1], 0);

	while (1) {
    if (HAL_SPI_TransmitReceive(&hspi2, tx_buf, rx_buf,
                                FRAME_LEN, HAL_MAX_DELAY) == HAL_OK) 
		{
        handle_frame();
    } 
		else 
		{
        spi_resync();
    }
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


/* USER CODE BEGIN 4 */
static uint32_t unpack_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void pack_u32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static uint32_t reg_read(uint8_t addr) {
    switch (addr) {
        case REG_DEVICE_ID:   return 0xAC00ACC0u;
        case REG_FW_VERSION:  return 0x00010200u;
        case REG_STATUS:      return reg_status;
        case REG_CONTROL:     return reg_control;
        case REG_SAMPLE_RATE: return reg_sample_rate;
        case REG_NOP:         return 0;
        default:
            reg_status |= ST_CMD_ERR;
            return 0;
    }
}

static void reg_write(uint8_t addr, uint32_t val) {
    switch (addr) {
        case REG_CONTROL:
            if (val & 0x02u) {                    /* CLEAR_FLAGS */
                reg_status &= ~(ST_CMD_ERR | ST_RANGE_ERR | ST_SPI_RESYNC);
                val &= ~0x02u;
            }
            reg_control = val & 0x01u;
            if (reg_control & 0x01u) reg_status |=  ST_RUNNING;
            else                     reg_status &= ~ST_RUNNING;
            break;

        case REG_SAMPLE_RATE:
            if (val >= 1u && val <= 10000u) reg_sample_rate = val;
            else                            reg_status |= ST_RANGE_ERR;
            break;

        default:
            reg_status |= ST_CMD_ERR;             /* 只读寄存器或未定义地址 */
            break;
    }
}

/* 处理刚收到的帧，并填好【下一帧】要发的内容 */
static void handle_frame(void) {
    uint8_t  cmd  = rx_buf[0];
    uint8_t  addr = cmd & CMD_ADDR_MASK;
    uint32_t resp = 0;

    if (cmd & CMD_WRITE_FLAG) {
        reg_write(addr, unpack_u32(&rx_buf[1]));
        resp = 0;
    } else {
        resp = reg_read(addr);
    }
    tx_buf[0] = cmd;              /* echo */
    pack_u32(&tx_buf[1], resp);
}

static void spi_resync(void) {
    volatile uint32_t tmp;
    __HAL_SPI_DISABLE(&hspi2);
    tmp = hspi2.Instance->DR;      /* 读 DR 再读 SR 清 OVR */
    tmp = hspi2.Instance->SR;
    (void)tmp;
    __HAL_SPI_CLEAR_OVRFLAG(&hspi2);
    hspi2.State = HAL_SPI_STATE_READY;
    hspi2.ErrorCode = HAL_SPI_ERROR_NONE;
    reg_status |= ST_SPI_RESYNC;
    __HAL_SPI_ENABLE(&hspi2);
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
