<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐、电池参数等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

### 特斯拉卡片钥匙界面字库

[`fonts/tesla_symbols.txt`](fonts/tesla_symbols.txt) 是特斯拉卡片钥匙应用的字符清单：
ASCII `0x20-0x7E` 加上 `main/` 下所有显示字符串字面量中的非 ASCII 码点，以及车牌号这类
运行时输入的字符（省级简称与「使领警学港澳挂」，共 300 个）。
`fonts/tesla_font_16.c`、`fonts/tesla_font_20.c`、`fonts/tesla_font_26.c` 由
`lv_font_conv` 1.5.3 基于微软雅黑 Regular 生成（系统 `msyh.ttc` 的 face 0）。
`fonts/tesla_font_lock.c` 是 36 px 子集，只包含主页大字号锁车状态用到的 11 个汉字
（方案 C 极简主页），用于控制 Flash 占用。
**微软雅黑是 Windows 的商业字体，不可随固件再分发**：生成的位图源码仅限本机/自用固件；
公开发布固件或本仓库前，请换回 Noto Sans SC（SIL OFL 1.1）等允许再分发的字体。
字体原文件不入库，仅提交生成后的位图源码。

- 字号与格式：16/20/26 px（完整字符清单）与 36 px（锁车大字子集），4 bpp，
  `--format lvgl --no-compress`，适配 LVGL 9。
- 生成命令（对每个字号重复，bash 语法；`msyh-face0.ttf` 为 `C:\Windows\Fonts\msyh.ttc`
  的 face 0 提取文件）：

  ```bash
  lv_font_conv --font msyh-face0.ttf --range 0x20-0x7E \
    --symbols "$(cat assets/fonts/tesla_symbols.txt)" --size 16 --bpp 4 \
    --format lvgl --no-compress --lv-font-name tesla_font_16 \
    --lv-include lvgl.h --output assets/fonts/tesla_font_16.c
  ```

  锁车大字子集用同样命令，改为 `--size 36`，`--symbols` 取
  `"$(cat assets/fonts/tesla_lock_symbols.txt)"`。文案变化时同步扩充该清单。

- 目标路径：编译进 `main` 组件，变量名为 `tesla_font_16/20/26` 与
  `tesla_font_lock`。
- 覆盖门禁：修改界面文案后运行 `python tools/check_glyphs.py`，失败即需重新生成字库。
- Flash 影响：四份源码合计约 1.22 MB；链接后的应用镜像约 1.71 MB，8 MB 应用分区仍余约 78%。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |
| [`images/car-model-3.png`](images/car-model-3.png) | 216 × 100，PNG RGB | 卡片钥匙主页的 2024 款 Model 3（焕新版，后轮驱动标准版）写实车头正面图：细长水平大灯、无前标平滑车头、单条下部进气口。用内置图像生成工具按实车正面出图，再以亮度阈值加四角泛洪去除背景，只保留最大连通区域（车身）以剔除角落水印，车身等比缩到 96 px 高后合成到 `#14181F` 面板底色上；转换为 RGB565（`car_model_3`）存入 [`main/car_art.c`](../main/car_art.c)，配置车型为 Model 3 时显示。 |
| [`images/car-model-y.png`](images/car-model-y.png) | 216 × 100，PNG RGB | 同一主页使用的 Model Y 写实车头正面图（车身更高），处理方式相同，转换为 `car_model_y`；配置车型为 Model Y 时显示。两张图均为静态图，锁车与车门状态显示在图片上方的文字卡片中。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。

## 电池参数（battery）

[`battery/wanli-500mah-profile2-xil40x-20261020.txt`](battery/wanli-500mah-profile2-xil40x-20261020.txt)
保存万锂新能源 500 mAh 电芯的 CW2017 参数（profile2、XIL40X、2026-10-20）。
该文件是项目提供的 `喜梨_智能工卡_万锂新能源500mAh_profile2_XIL40X_20261020.txt`
的原样副本，按维护者要求保留用于本次固件更新；原文件没有附带单独的许可说明。

格式为 80 行两位十六进制数字，每行一个字节，按寄存器顺序排列。
BSP 在 `components/bsp/src/bsp_battery.c` 中嵌入这 80 字节，主机检查会验证
源文件及数组内容完全一致。文本文件 SHA-256：
`7ea96eba4634f4495a37c25ffacd54c1deaa0693b162c892d671a2ba68460919`。
解码后 80 字节 SHA-256：
`fa7365671093ef449c5bdcfb4adb279e3d275f888109f50b6b2f21e9b96220d5`。
这是电量计参数，不是充电配置，也不能通用于所有 500 mAh 电芯；详见
[硬件指南](../docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md#9-cw2017-电池计)。
