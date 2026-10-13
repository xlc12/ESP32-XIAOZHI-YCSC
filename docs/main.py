"""
RP2040 WS2812 LED Control - MicroPython
通过USB串口接收指令控制LED效果

指令格式:
  template,<id>    # 使用预设模板 (0-11), -1为关闭
  0               # 关闭（同-1）
  fsr_enable,0/1 | fsr_calibrate,0/1 | fsr_recal | fsr_params,<d>,<b>,<pn>,<rn> | fsr_baseline,<n>  # 压力传感器（可选）
  fsr_threshold,<low>,<high> | fsr_invert,0/1  # 已弃用，兼容 no-op

模板列表:
  0=idle 1=listening 2=thinking 3=speaking 4=notification
  5=sleep 6=voiceid 7=meditation 8=error
  9=night_idle 10=night_listen 11=breath_478
"""

import machine
import neopixel
import utime
from math import sin, pi

# ========== 配置 ==========
LED_PIN = 0          # GPIO0
LED_COUNT = 16       # 物理灯珠数量（WS2812 整条链长度）
# 逻辑活跃区：头部禁 3 颗（index 0–2）、尾部禁 4 颗（index 12–15），中间 9 颗参与灯效
LED_ACTIVE_FIRST = 3
LED_ACTIVE_LAST = 11
LED_ACTIVE_COUNT = LED_ACTIVE_LAST - LED_ACTIVE_FIRST + 1

# 初始化LED
np = neopixel.NeoPixel(machine.Pin(LED_PIN), LED_COUNT)

# 当前效果
current_template = 0  # 默认为idle
effect_running = True


# ========== PIR 传感器配置 ==========
PIR_PIN = 5
pir_sensor = None  # 延迟初始化
pir_enabled = True
pir_state = 0
pir_last_state = 0
pir_last_motion_time = 0
pir_timeout_ms = 30000  # 30秒超时
pir_warmup_time = 60000  # 60秒预热时间
pir_ready = False
pir_start_time = 0


# ========== FSR 压力传感器（ADC，基线标定 + 上升沿确认）==========
FSR_ADC_PIN = 26
FSR_DEFAULT_CALIB_DURATION_MS = 3000
# 相对标定后截尾均值基线的动态门限（ADC raw，约 0..65535）
FSR_DEFAULT_DELTA = 6500
FSR_DEFAULT_PRESS_DELTA = FSR_DEFAULT_DELTA
FSR_DEFAULT_RELEASE_BAND = 4000
FSR_DEFAULT_PRESS_CONFIRM_N = 2
FSR_DEFAULT_RELEASE_CONFIRM_N = 2
# 标定离群剔除：(x-mean)^2 > 该值视为误按压污染点
FSR_CALIB_OUTLIER_VAR_THRESHOLD = 3000
# 稳定窗口：仅当最近 N 个样本 spread <= SPREAD_MAX 时纳入 baseline 候选
FSR_CALIB_STABLE_WINDOW = 6
FSR_CALIB_STABLE_SPREAD_MAX = 800
FSR_CALIB_MIN_STABLE_SAMPLES = 8
# 串口命令缓冲上限（非阻塞增量解析）
FSR_CMD_BUF_MAX = 128


class FSRDetector:
    """上电短时标定 + 稳定样本筛选 + 离群剔除 + 单阈值边沿确认；对外仅通过 update() 返回边沿事件。"""

    STATE_CALIBRATING = "CALIBRATING"
    STATE_READY_RELEASED = "READY_RELEASED"
    STATE_PRESS_CANDIDATE = "PRESS_CANDIDATE"
    STATE_PRESSED = "PRESSED"
    STATE_RELEASE_CANDIDATE = "RELEASE_CANDIDATE"

    def __init__(
        self,
        adc_pin=26,
        calib_duration_ms=FSR_DEFAULT_CALIB_DURATION_MS,
        calib_outlier_var_threshold=FSR_CALIB_OUTLIER_VAR_THRESHOLD,
        press_delta=FSR_DEFAULT_PRESS_DELTA,
        release_band=FSR_DEFAULT_RELEASE_BAND,
        press_confirm_n=FSR_DEFAULT_PRESS_CONFIRM_N,
        release_confirm_n=FSR_DEFAULT_RELEASE_CONFIRM_N,
        var_warn_threshold=FSR_CALIB_OUTLIER_VAR_THRESHOLD,
        calibrate_print_interval_ms=200,
    ):
        self._adc_pin = adc_pin
        self._adc = None
        self.calib_duration_ms = calib_duration_ms
        self.calib_outlier_var_threshold = calib_outlier_var_threshold
        self.press_delta = press_delta
        self.release_band = release_band
        self.press_confirm_n = press_confirm_n
        self.release_confirm_n = release_confirm_n
        self.var_warn_threshold = var_warn_threshold
        self._calibrate_print_interval_ms = calibrate_print_interval_ms

        self._enabled = True
        self._calibrate_print = False
        self._last_calibrate_print_ms = 0
        self._state = self.STATE_CALIBRATING
        self._baseline = -1
        self._calib_samples = []
        self._calib_start_ms = 0
        self._press_hit_count = 0
        self._release_hit_count = 0
        self._calib_announced = False

    def _init_adc(self):
        if self._adc is None:
            self._adc = machine.ADC(self._adc_pin)
            print(f"FSR: Initialized on GPIO{self._adc_pin} (ADC)")

    def start_calibration(self, now_ms):
        self._init_adc()
        self._state = self.STATE_CALIBRATING
        self._calib_samples = []
        self._calib_start_ms = now_ms
        self._press_hit_count = 0
        self._release_hit_count = 0
        self._calib_announced = False

    def request_recal(self, now_ms=None):
        if now_ms is None:
            now_ms = utime.ticks_ms()
        self.start_calibration(now_ms)

    def set_enabled(self, flag):
        self._enabled = bool(flag)

    def set_calibrate_print(self, flag):
        self._calibrate_print = bool(flag)

    def set_params(self, press_delta=None, release_band=None, press_n=None, release_n=None):
        if press_delta is not None:
            self.press_delta = int(press_delta)
        if release_band is not None:
            self.release_band = int(release_band)
        if press_n is not None:
            self.press_confirm_n = int(press_n)
        if release_n is not None:
            self.release_confirm_n = int(release_n)

    def set_baseline(self, value):
        self._baseline = int(value)
        self._state = self.STATE_READY_RELEASED
        self._press_hit_count = 0
        self._release_hit_count = 0

    @staticmethod
    def _mean(samples):
        n = len(samples)
        if n == 0:
            return 0
        return sum(samples) // n

    @staticmethod
    def _filter_outliers(samples, var_threshold):
        """迭代剔除 (x-mean)^2 > var_threshold 的离群点（误按压污染）。"""
        n = len(samples)
        if n == 0:
            return []
        if n == 1:
            return [samples[0]]
        working = list(samples)
        for _ in range(8):
            mean = FSRDetector._mean(working)
            kept = []
            for x in working:
                d = x - mean
                if d * d <= var_threshold:
                    kept.append(x)
            if len(kept) == len(working):
                return kept
            if len(kept) == 0:
                return working
            working = kept
        return working

    @staticmethod
    def _variance(samples):
        n = len(samples)
        if n < 2:
            return 0
        mean = sum(samples) // n
        acc = 0
        for x in samples:
            d = x - mean
            acc += d * d
        return acc // n

    @staticmethod
    def _select_stable_samples(samples, window, spread_max):
        """滑动窗口：仅 spread <= spread_max 的窗口内样本进入候选集。"""
        n = len(samples)
        if n < window:
            return []
        stable = []
        start = window - 1
        idx = start
        while idx < n:
            w = samples[idx - window + 1 : idx + 1]
            spread = max(w) - min(w)
            if spread <= spread_max:
                stable.extend(w)
            idx += 1
        return stable

    def _read_raw(self):
        self._init_adc()
        return self._adc.read_u16()

    def _finish_calibration(self, now_ms):
        raw_n = len(self._calib_samples)
        stable_candidates = self._select_stable_samples(
            self._calib_samples,
            FSR_CALIB_STABLE_WINDOW,
            FSR_CALIB_STABLE_SPREAD_MAX,
        )
        used_stable = len(stable_candidates) >= FSR_CALIB_MIN_STABLE_SAMPLES
        source = stable_candidates if used_stable else self._calib_samples
        if not used_stable:
            print("dbg,fsr_calib_warn reason=unstable_samples n_stable={}".format(
                len(stable_candidates)
            ))
        filtered = self._filter_outliers(source, self.calib_outlier_var_threshold)
        if len(filtered) == 0 and len(self._calib_samples) > 0:
            filtered = [self._calib_samples[-1]]
        baseline = self._mean(filtered)
        var = self._variance(filtered)
        self._baseline = baseline
        self._state = self.STATE_READY_RELEASED
        self._press_hit_count = 0
        self._release_hit_count = 0
        kept_n = len(filtered)
        dropped = len(source) - kept_n
        print(
            "dbg,fsr_calib_done baseline={} var={} n={} n_raw={} n_stable={} dropped={} stable_path={}".format(
                baseline,
                var,
                kept_n,
                raw_n,
                len(stable_candidates),
                dropped,
                1 if used_stable else 0,
            )
        )
        if var > self.var_warn_threshold:
            print(f"dbg,fsr_calib_warn var={var} threshold={self.var_warn_threshold}")

    def _tick_calibrating(self, now_ms, raw):
        if not self._calib_announced:
            print(f"dbg,fsr_calib_start ms={self.calib_duration_ms}")
            self._calib_announced = True
        self._calib_samples.append(raw)
        if utime.ticks_diff(now_ms, self._calib_start_ms) >= self.calib_duration_ms:
            if len(self._calib_samples) > 0:
                self._finish_calibration(now_ms)
            else:
                self._baseline = raw
                self._state = self.STATE_READY_RELEASED
                print(f"dbg,fsr_calib_done baseline={self._baseline} var=0 n=0")
        return None

    def _is_press_sample(self, raw):
        if self._baseline < 0:
            return False
        return raw > self._baseline + self.press_delta

    def _is_release_sample(self, raw):
        if self._baseline < 0:
            return False
        # 单侧释放：raw 低于 baseline+release_band 即视为回到释放区（无下界）
        return raw < self._baseline + self.release_band

    def _tick_detection(self, now_ms, raw):
        if self._state == self.STATE_READY_RELEASED:
            if self._is_press_sample(raw):
                if self.press_confirm_n <= 1:
                    self._state = self.STATE_PRESSED
                    self._release_hit_count = 0
                    return ("press", now_ms)
                self._state = self.STATE_PRESS_CANDIDATE
                self._press_hit_count = 1
            else:
                self._press_hit_count = 0
        elif self._state == self.STATE_PRESS_CANDIDATE:
            if self._is_press_sample(raw):
                self._press_hit_count += 1
                if self._press_hit_count >= self.press_confirm_n:
                    self._state = self.STATE_PRESSED
                    self._release_hit_count = 0
                    return ("press", now_ms)
            else:
                self._state = self.STATE_READY_RELEASED
                self._press_hit_count = 0
        elif self._state == self.STATE_PRESSED:
            if self._is_release_sample(raw):
                if self.release_confirm_n <= 1:
                    self._state = self.STATE_READY_RELEASED
                    self._press_hit_count = 0
                    return ("release", now_ms)
                self._state = self.STATE_RELEASE_CANDIDATE
                self._release_hit_count = 1
            else:
                self._release_hit_count = 0
        elif self._state == self.STATE_RELEASE_CANDIDATE:
            if self._is_release_sample(raw):
                self._release_hit_count += 1
                if self._release_hit_count >= self.release_confirm_n:
                    self._state = self.STATE_READY_RELEASED
                    self._press_hit_count = 0
                    return ("release", now_ms)
            else:
                self._state = self.STATE_PRESSED
                self._release_hit_count = 0
        return None

    def update(self, now_ms):
        if not self._enabled:
            return None
        raw = self._read_raw()
        if self._calibrate_print:
            if utime.ticks_diff(now_ms, self._last_calibrate_print_ms) >= self._calibrate_print_interval_ms:
                print(f"dbg,fsr_raw={raw}")
                self._last_calibrate_print_ms = now_ms
        if self._state == self.STATE_CALIBRATING:
            return self._tick_calibrating(now_ms, raw)
        if self._baseline < 0:
            return None
        return self._tick_detection(now_ms, raw)

    def stats(self):
        return {
            "state": self._state,
            "baseline": self._baseline,
            "enabled": self._enabled,
        }


fsr = FSRDetector(adc_pin=FSR_ADC_PIN)


# ========== 模板配置（严格按照Oracube RGB规范v1.0） ==========
MODE_TEMPLATES = {
    0: {
        "name": "idle",
        "color": (245, 239, 216),
        "anim": "breath",
        "duration": 3.5,
        "brightness_min": 0.15,
        "brightness_max": 0.50
    },
    1: {
        "name": "listening",
        "color": (90, 180, 240),
        "anim": "gradient_center",
        "duration": 1.1,
        "brightness_min": 0.20,
        "brightness_max": 0.80
    },
    2: {
        "name": "thinking",
        "base_color": (122, 88, 0),
        "sweep_color": (247, 199, 82),
        "anim": "sweep",
        "duration": 1.1,
        "sweep_width": 0.22,
        "base_brightness": 0.20
    },
    3: {
        "name": "speaking",
        "color": (94, 207, 142),
        "anim": "gradient_center",
        "duration": 0.85,
        "brightness_min": 0.20,
        "brightness_max": 0.85
    },
    4: {
        "name": "notification",
        "color": (255, 140, 66),
        "anim": "double_blink",
        "duration": 2.4
    },
    5: {
        "name": "sleep",
        "color": (112, 128, 152),
        "anim": "breath",
        "duration": 5.5,
        "brightness_min": 0.05,
        "brightness_max": 0.15
    },
    6: {
        "name": "voiceid",
        "base_color": (10, 112, 112),
        "sweep_color": (42, 219, 212),
        "anim": "sweep",
        "duration": 1.5,
        "sweep_width": 0.18,
        "base_brightness": 0.30
    },
    7: {
        "name": "meditation",
        "color": (155, 127, 223),
        "anim": "expand_contract",
        "duration": 5.5,
        "brightness_min": 0.30,
        "brightness_max": 0.90
    },
    8: {
        "name": "error",
        "color": (232, 80, 80),
        "anim": "rapid_blink",
        "duration": 2.0
    },
    9: {
        "name": "night_idle",
        "color": (192, 104, 24),
        "anim": "breath",
        "duration": 4.0,
        "brightness_min": 0.18,
        "brightness_max": 0.36
    },
    10: {
        "name": "night_listen",
        "color": (200, 48, 48),
        "anim": "gradient_center",
        "duration": 1.1,
        "brightness_min": 0.20,
        "brightness_max": 0.52
    },
    11: {
        "name": "breath_478",
        "color": (155, 127, 223),
        "anim": "breath_478",
        "inhale": 4.0,
        "hold": 7.0,
        "exhale": 8.0,
        "brightness_min": 0.30,
        "brightness_max": 0.90
    }
}


# ========== 工具函数 ==========
def set_brightness(rgb, brightness):
    """调整RGB亮度"""
    r, g, b = rgb
    return (int(r * brightness), int(g * brightness), int(b * brightness))


def blend_colors(c1, c2, ratio):
    """在两个颜色之间插值"""
    return (
        int(c1[0] + (c2[0] - c1[0]) * ratio),
        int(c1[1] + (c2[1] - c1[1]) * ratio),
        int(c1[2] + (c2[2] - c1[2]) * ratio)
    )


def active_led_range():
    """参与灯效的 LED 索引（含首尾）"""
    return range(LED_ACTIVE_FIRST, LED_ACTIVE_LAST + 1)


def active_center():
    """活跃区几何中心（用于对称渐变/展开）"""
    return (LED_ACTIVE_FIRST + LED_ACTIVE_LAST) / 2


def active_half_span():
    """活跃区半宽（用于距离归一化）"""
    return (LED_ACTIVE_LAST - LED_ACTIVE_FIRST) / 2


def is_active_led(index):
    return LED_ACTIVE_FIRST <= index <= LED_ACTIVE_LAST


def clear_inactive_leds():
    """活跃区外（含头部/尾部禁亮颗）强制熄灭"""
    for i in range(LED_COUNT):
        if not is_active_led(i):
            np[i] = (0, 0, 0)


def write_pixels():
    """刷新灯带并保证首尾禁亮"""
    clear_inactive_leds()
    np.write()


def clear_all_pixels():
    """熄灭全部物理灯珠"""
    for i in range(LED_COUNT):
        np[i] = (0, 0, 0)
    np.write()


def get_gradient_brightness(pos):
    """计算中心渐变亮度（中心最亮，活跃区两端最暗但不灭）"""
    if not is_active_led(pos):
        return 0
    center = active_center()
    # 以活跃区真实边缘到中心的距离归一化（短灯带时避免首尾 gradient=0 完全不亮）
    edge_dist = max(center - LED_ACTIVE_FIRST, LED_ACTIVE_LAST - center)
    if edge_dist <= 0:
        return 1.0
    distance = abs(pos - center) / edge_dist
    edge_floor = 0.22
    return edge_floor + (1.0 - edge_floor) * max(0.0, 1.0 - distance)


def set_all(color):
    """设置所有活跃 LED 颜色"""
    for i in active_led_range():
        np[i] = color
    write_pixels()


# ========== PIR 传感器函数 ==========
def init_pir():
    """初始化PIR传感器"""
    global pir_sensor, pir_state, pir_last_state, pir_start_time
    pir_sensor = machine.Pin(PIR_PIN, machine.Pin.IN, machine.Pin.PULL_DOWN)
    pir_state = pir_sensor.value()
    pir_last_state = pir_state
    pir_start_time = utime.ticks_ms()
    print(f"PIR: Initialized on GPIO{PIR_PIN}")
    print(f"PIR: Warmup period ({pir_warmup_time/1000}s) started...")


def send_pir_event(state, timestamp):
    """发送PIR事件到上位机"""
    if pir_enabled:
        print(f"pir,{state},{timestamp}")


def check_pir_timeout():
    """检查PIR超时，返回True如果应该触发超时"""
    global pir_state, pir_last_motion_time

    if not pir_ready or not pir_enabled:
        return False

    if pir_state == 1:  # 当前检测到人
        elapsed = utime.ticks_diff(utime.ticks_ms(), pir_last_motion_time)
        if elapsed > pir_timeout_ms:
            pir_state = 0
            pir_last_state = 0
            send_pir_event(0, utime.ticks_ms())
            return True
    return False


def poll_pir():
    """轮询PIR传感器状态"""
    global pir_state, pir_last_state, pir_last_motion_time, pir_ready

    if pir_sensor is None or not pir_enabled:
        return

    timestamp = utime.ticks_ms()

    # 检查预热是否完成
    if not pir_ready:
        if timestamp > pir_warmup_time:
            pir_ready = True
            print("PIR: Warmup complete, motion detection active")
        return

    # 读取当前状态
    current = pir_sensor.value()

    # 状态变化检测
    if current != pir_last_state:
        pir_last_state = current
        if current:
            pir_state = 1
            pir_last_motion_time = timestamp
            send_pir_event(1, timestamp)
            # 人来了自动切换到 listening 效果
            current_template = 1
            print("PIR: Auto-switch to listening")
        else:
            send_pir_event(0, timestamp)


# ========== FSR 驱动层（协议输出）==========
def _emit_fsr_event(evt):
    """将 FSRDetector.update() 返回值转为 fsr,1/0,<ticks> 行协议。"""
    if evt is None:
        return
    kind, ts = evt
    print(f"fsr,{1 if kind == 'press' else 0},{ts}")


# ========== 效果函数 ==========
def effect_idle():
    """待机 - 暖白呼吸"""
    template = MODE_TEMPLATES[0]
    c1 = set_brightness(template["color"], template["brightness_min"])
    c2 = set_brightness(template["color"], template["brightness_max"])
    duration = template["duration"]

    while effect_running and current_template == 0:
        steps = int(duration * 50)
        for step in range(steps):
            if not effect_running or current_template != 0:
                break
            intensity = (sin(step / steps * 2 * pi) + 1) / 2
            current_color = blend_colors(c1, c2, intensity)
            set_all(current_color)
            utime.sleep(0.02)


def effect_gradient_center(mode_id):
    """中心渐变呼吸（listening/speaking/night_listen）"""
    template = MODE_TEMPLATES[mode_id]
    base_color = template["color"]
    duration = template["duration"]
    brightness_min = template.get("brightness_min", 0.20)
    brightness_max = template["brightness_max"]

    while effect_running and current_template == mode_id:
        steps = int(duration * 50)
        for step in range(steps):
            if not effect_running or current_template != mode_id:
                break
            intensity = (sin(step / steps * 2 * pi) + 1) / 2
            brightness = brightness_min + (brightness_max - brightness_min) * intensity

            for i in active_led_range():
                gradient = get_gradient_brightness(i)
                led_brightness = brightness * gradient
                np[i] = set_brightness(base_color, led_brightness)
            write_pixels()
            utime.sleep(0.02)


def effect_sweep(mode_id):
    """扫描效果（thinking/voiceid）"""
    template = MODE_TEMPLATES[mode_id]
    base_color = set_brightness(template["base_color"], template["base_brightness"])
    sweep_color = template["sweep_color"]
    duration = template["duration"]
    sweep_width = max(1, int(LED_ACTIVE_COUNT * template["sweep_width"]))

    while effect_running and current_template == mode_id:
        steps = int(duration * 50)
        for step in range(steps):
            if not effect_running or current_template != mode_id:
                break
            pos = int((step / steps) * LED_ACTIVE_COUNT * 2) % LED_ACTIVE_COUNT

            for i in active_led_range():
                np[i] = base_color

            for i in range(sweep_width):
                led_pos = LED_ACTIVE_FIRST + ((pos + i) % LED_ACTIVE_COUNT)
                ratio = (sweep_width - i) / sweep_width
                np[led_pos] = (
                    int(sweep_color[0] * ratio),
                    int(sweep_color[1] * ratio),
                    int(sweep_color[2] * ratio)
                )

            write_pixels()
            utime.sleep(0.02)


def effect_double_blink(mode_id):
    """双闪效果（notification）"""
    template = MODE_TEMPLATES[mode_id]
    flash_color = template["color"]
    duration = template["duration"]
    off_color = (15, 15, 15)

    blink_time = duration / 5

    while effect_running and current_template == mode_id:
        set_all(flash_color)
        utime.sleep(blink_time * 0.72)
        set_all(off_color)
        utime.sleep(blink_time * 0.54)
        set_all(flash_color)
        utime.sleep(blink_time * 0.72)
        set_all(off_color)
        utime.sleep(blink_time * 0.18)
        utime.sleep(0.1)


def effect_rapid_blink(mode_id):
    """急促双闪（error）"""
    template = MODE_TEMPLATES[mode_id]
    flash_color = template["color"]
    off_color = (15, 15, 15)

    blink_time = 0.2

    while effect_running and current_template == mode_id:
        set_all(flash_color)
        utime.sleep(blink_time)
        set_all(off_color)
        utime.sleep(blink_time)
        set_all(flash_color)
        utime.sleep(blink_time)
        set_all(off_color)
        utime.sleep(blink_time * 1.5)
        utime.sleep(0.1)


def effect_breath(mode_id):
    """呼吸效果（sleep/night_idle）"""
    template = MODE_TEMPLATES[mode_id]
    c1 = set_brightness(template["color"], template["brightness_min"])
    c2 = set_brightness(template["color"], template["brightness_max"])
    duration = template["duration"]

    while effect_running and current_template == mode_id:
        steps = int(duration * 50)
        for step in range(steps):
            if not effect_running or current_template != mode_id:
                break
            intensity = (sin(step / steps * 2 * pi) + 1) / 2
            current_color = blend_colors(c1, c2, intensity)
            set_all(current_color)
            utime.sleep(0.02)


def effect_expand_contract():
    """展开收缩（meditation）- 带丝滑边缘"""
    template = MODE_TEMPLATES[7]
    center = active_center()
    half_span = active_half_span()
    duration = template["duration"]
    brightness_min = template["brightness_min"]
    brightness_max = template["brightness_max"]
    color = template["color"]

    while effect_running and current_template == 7:
        steps = int(duration * 50)
        for step in range(steps):
            if not effect_running or current_template != 7:
                break
            intensity = (sin(step / steps * 2 * pi) + 1) / 2
            brightness = brightness_min + (brightness_max - brightness_min) * intensity
            expand_ratio = intensity

            for i in active_led_range():
                distance = abs(i - center) / half_span if half_span > 0 else 0
                distance_brightness = max(0, 1.0 - distance * 0.6)
                edge_distance = distance - expand_ratio

                if edge_distance <= 0:
                    led_brightness = brightness * distance_brightness
                    np[i] = set_brightness(color, led_brightness)
                elif edge_distance <= 0.15:
                    fade_ratio = 1.0 - (edge_distance / 0.15)
                    led_brightness = brightness * distance_brightness * fade_ratio
                    np[i] = set_brightness(color, led_brightness)
                else:
                    np[i] = (0, 0, 0)

            write_pixels()
            utime.sleep(0.02)


def effect_breath_478():
    """4-7-8呼吸训练 - 带丝滑边缘"""
    template = MODE_TEMPLATES[11]
    center = active_center()
    half_span = active_half_span()
    c_min = set_brightness(template["color"], template["brightness_min"])
    c_max = set_brightness(template["color"], template["brightness_max"])
    inhale_time = template["inhale"]
    hold_time = template["hold"]
    exhale_time = template["exhale"]

    while effect_running and current_template == 11:
        # 吸气
        inhale_steps = int(inhale_time * 50)
        for step in range(inhale_steps):
            if not effect_running or current_template != 11:
                break
            progress = step / inhale_steps
            current_color = blend_colors(c_min, c_max, progress)

            for i in active_led_range():
                distance = abs(i - center) / half_span if half_span > 0 else 0
                edge_distance = distance - progress

                if edge_distance <= 0:
                    distance_brightness = max(0.3, 1.0 - distance * 0.5)
                    np[i] = set_brightness(current_color, distance_brightness)
                elif edge_distance <= 0.2:
                    fade_ratio = 1.0 - (edge_distance / 0.2)
                    distance_brightness = max(0.3, 1.0 - distance * 0.5)
                    np[i] = set_brightness(current_color, distance_brightness * fade_ratio)
                else:
                    np[i] = (0, 0, 0)

            write_pixels()
            utime.sleep(0.02)

        # 屏息
        hold_steps = int(hold_time * 50)
        for _ in range(hold_steps):
            if not effect_running or current_template != 11:
                break
            set_all(c_max)
            utime.sleep(0.02)

        # 呼气
        exhale_steps = int(exhale_time * 50)
        for step in range(exhale_steps):
            if not effect_running or current_template != 11:
                break
            progress = 1.0 - (step / exhale_steps)
            current_color = blend_colors(c_min, c_max, progress)

            for i in active_led_range():
                distance = abs(i - center) / half_span if half_span > 0 else 0
                edge_distance = distance - progress

                if edge_distance <= 0:
                    distance_brightness = max(0.3, 1.0 - distance * 0.5)
                    np[i] = set_brightness(current_color, distance_brightness)
                elif edge_distance <= 0.2:
                    fade_ratio = 1.0 - (edge_distance / 0.2)
                    distance_brightness = max(0.3, 1.0 - distance * 0.5)
                    np[i] = set_brightness(current_color, distance_brightness * fade_ratio)
                else:
                    np[i] = (0, 0, 0)

            write_pixels()
            utime.sleep(0.02)


def effect_off():
    """关闭所有LED（含首尾禁亮区）"""
    clear_all_pixels()


# ========== 效果运行器 ==========
def run_effect():
    """运行当前效果"""
    global effect_running

    while True:
        try:
            if current_template == -1:
                effect_off()
                utime.sleep(0.1)
            elif current_template == 0:
                effect_idle()
            elif current_template == 1:
                effect_gradient_center(1)
            elif current_template == 2:
                effect_sweep(2)
            elif current_template == 3:
                effect_gradient_center(3)
            elif current_template == 4:
                effect_double_blink(4)
            elif current_template == 5:
                effect_breath(5)
            elif current_template == 6:
                effect_sweep(6)
            elif current_template == 7:
                effect_expand_contract()
            elif current_template == 8:
                effect_rapid_blink(8)
            elif current_template == 9:
                effect_breath(9)
            elif current_template == 10:
                effect_gradient_center(10)
            elif current_template == 11:
                effect_breath_478()
            else:
                utime.sleep(0.1)

        except Exception as e:
            utime.sleep(0.1)


# ========== 命令解析 ==========
def handle_command_line(line):
    """解析单行上位机命令（与 FSR 轮询解耦，可单独测试）。"""
    global current_template, pir_enabled, pir_timeout_ms

    if not line:
        return

    # PIR 配置命令
    if line.startswith("pir_enable,"):
        try:
            val = int(line.split(",")[1])
            pir_enabled = bool(val)
            print(f"PIR: {'Enabled' if pir_enabled else 'Disabled'}")
        except:
            print("Error: Invalid pir_enable command")

    elif line.startswith("pir_timeout,"):
        try:
            val = int(line.split(",")[1])
            pir_timeout_ms = val
            print(f"PIR: Timeout set to {val}ms")
        except:
            print("Error: Invalid pir_timeout command")

    elif line.startswith("fsr_enable,"):
        try:
            val = int(line.split(",")[1])
            fsr.set_enabled(bool(val))
            print(f"FSR: {'Enabled' if val else 'Disabled'}")
        except:
            print("Error: Invalid fsr_enable command")

    elif line.startswith("fsr_calibrate,"):
        try:
            val = int(line.split(",")[1])
            fsr.set_calibrate_print(bool(val))
            print(f"FSR: Calibrate {'ON' if val else 'OFF'}")
        except:
            print("Error: Invalid fsr_calibrate command")

    elif line == "fsr_recal":
        fsr.request_recal(utime.ticks_ms())
        print("FSR: Recalibration started ({}s baseline window)".format(
            fsr.calib_duration_ms // 1000
        ))

    elif line.startswith("fsr_params,"):
        try:
            parts = line.split(",")
            press_delta = int(parts[1])
            release_band = int(parts[2])
            press_n = int(parts[3])
            release_n = int(parts[4])
            fsr.set_params(press_delta, release_band, press_n, release_n)
            print(
                "FSR: Params delta={} band={} press_n={} release_n={}".format(
                    fsr.press_delta,
                    fsr.release_band,
                    fsr.press_confirm_n,
                    fsr.release_confirm_n,
                )
            )
        except:
            print("Error: Invalid fsr_params (use fsr_params,<press_delta>,<release_band>,<press_n>,<release_n>)")

    elif line.startswith("fsr_baseline,"):
        try:
            val = int(line.split(",")[1])
            fsr.set_baseline(val)
            print(f"FSR: Baseline set to {val}")
        except:
            print("Error: Invalid fsr_baseline command")

    elif line.startswith("fsr_threshold,"):
        print("Warning: fsr_threshold is deprecated; use fsr_params or fsr_recal")

    elif line.startswith("fsr_invert,"):
        print("Warning: fsr_invert is deprecated; use fsr_params or fsr_recal")

    elif line.startswith("template,"):
        try:
            mode_id = int(line.split(",")[1])
            if -1 <= mode_id <= 11:
                current_template = mode_id
                if mode_id == -1:
                    print("Template: OFF")
                else:
                    template = MODE_TEMPLATES[mode_id]
                    print(f"Template: {template['name']}")
            else:
                print("Error: Template ID must be 0-11 or -1")
        except:
            print("Error: Invalid template command")

    elif line == "0":
        current_template = -1
        print("Template: OFF")


def _drain_serial_commands(poller, cmd_buf):
    """非阻塞读取 stdin，按行累积到 cmd_buf；返回更新后的缓冲。"""
    import sys

    while poller.poll(0):
        chunk = sys.stdin.read(1)
        if not chunk:
            break
        ch = chunk[0] if isinstance(chunk, str) else chr(chunk)
        if ch in "\n\r":
            line = cmd_buf.strip()
            cmd_buf = ""
            if line:
                handle_command_line(line)
        else:
            if len(cmd_buf) < FSR_CMD_BUF_MAX:
                cmd_buf += ch
            else:
                cmd_buf = ""
    return cmd_buf


def read_commands():
    """主循环：PIR + FSR 每轮必跑；串口命令增量解析，不阻塞 FSR 采样。"""
    import sys
    import uselect

    poller = uselect.poll()
    poller.register(sys.stdin, uselect.POLLIN)
    cmd_buf = ""

    while True:
        poll_pir()
        _emit_fsr_event(fsr.update(utime.ticks_ms()))
        cmd_buf = _drain_serial_commands(poller, cmd_buf)
        utime.sleep_ms(1)


# ========== 主程序 ==========
def main():
    global current_template, pir_last_motion_time

    effect_off()

    print("RP2040 LED Control Started")
    print(
        "LED strip: physical=%d active=%d (indices %d-%d)"
        % (LED_COUNT, LED_ACTIVE_COUNT, LED_ACTIVE_FIRST, LED_ACTIVE_LAST)
    )
    print("Template Commands: template,0-11 (or -1 for OFF)")
    print("PIR Commands: pir_enable,0/1 | pir_timeout,<ms>")
    print("FSR Commands: fsr_enable,0/1 | fsr_calibrate,0/1 | fsr_recal | fsr_params,<d>,<b>,<pn>,<rn> | fsr_baseline,<n>")
    print("Default: template,0 (idle)")
    print(f"Current: {MODE_TEMPLATES[0]['name']}")

    # 初始化PIR传感器
    init_pir()
    pir_last_motion_time = utime.ticks_ms()

    fsr.start_calibration(utime.ticks_ms())

    import _thread
    _thread.start_new_thread(run_effect, ())
    read_commands()


if __name__ == "__main__":
    main()
