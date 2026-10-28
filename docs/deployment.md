# 部署说明

1. 先准备已有 Hermes 网关 API：局域网可达，Bearer 密钥有效；不要将该接口直接暴露到公网。
2. NAS 服务只读挂载 Hermes 配置目录到 `/hermes`，从其中 `.env` 读取 API_SERVER_KEY。语音请求选用原网关的 DeepSeek provider。
3. 编辑 NAS `.env`、固件 `voice_config.h` 和电脑 `config.private.json`；三类令牌分别用于设备、管理诊断和 Codex 写入，不要混用。
4. `docker compose up -d` 启动后访问 `/health`。第一次下载依赖及小型流式 ASR 模型可能需要几分钟。
5. 手机连接设备热点配网，或长按 BOOT 两秒进入设置。配置写入仅接受设备热点网段。
6. 原 Hermes 凭据变化后需要重启本 NAS 服务使读取的配置更新；不要依赖旧进程中的快照。

默认使用在线晓晓女声，分句预取。`voice-settings.json` 可热更新 tts/voice/pitch/rate/model。
如果需要本地 VITS，先在已有依赖环境下执行 `PYTHONPATH=/app/deps python /app/prepare_local_tts.py` 下载模型，再将 tts 设为 local。
本地音色和在线音色不同，切换前应实听比较。完成通知缓存按音色参数区分。

股票管理和实时显示独立于电脑运行。Codex 状态需要电脑同步程序持续运行；超过25秒无心跳则显示离线，用量超过10分钟未更新标记过期。
Codex 状态从本地会话事件推导，未来客户端改动格式时可能需要适配。

所有可变数据（模型、对话历史、令牌、自选、状态快照、音频缓存）保存在 NAS 挂载目录，已通过 .gitignore 排除。
v1.0 尚无自动升级、完善的音量设置或稳定的第三方行情 SLA。
