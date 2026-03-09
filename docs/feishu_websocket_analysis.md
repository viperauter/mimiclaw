# 飞书 WebSocket 实现分析与整改计划

## 1. 飞书官方 WebSocket 实现分析

### 1.1 核心实现路径
**文件路径**：`/home/aotao/workspace/mimiclaw/third_party/lark-samples/.venv/lib/python3.12/site-packages/lark_oapi/ws/client.py`

### 1.2 连接流程
1. **动态获取连接 URL**：
   - 发送 HTTP POST 请求到 `https://open.feishu.cn/open-apis/bot/v3/endpoints`
   - 请求体包含 `AppID` 和 `AppSecret`
   - 从响应中获取 WebSocket 连接 URL

2. **建立 WebSocket 连接**：
   - 使用获取到的 URL 建立 WebSocket 连接
   - 连接 URL 包含 `device_id` 和 `service_id` 等参数

3. **消息处理循环**：
   - 启动消息接收循环
   - 启动 PING/PONG 心跳循环

### 1.3 消息格式
- **使用 Protocol Buffers** 序列化消息
- **消息类型**：
  - 控制帧（CONTROL）：PING/PONG
  - 数据帧（DATA）：事件消息

### 1.4 认证方式
- 使用 `AppID` 和 `AppSecret` 获取连接 URL
- 连接 URL 中包含认证信息
- 不需要额外的 `tenant_access_token`

### 1.5 关键代码分析

**获取连接 URL**：
```python
def _get_conn_url(self) -> str:
    response = requests.post(
        self._domain + GEN_ENDPOINT_URI,
        headers={"locale": "zh"},
        json={"AppID": self._app_id, "AppSecret": self._app_secret},
    )
    # 处理响应...
    return data.URL
```

**消息处理**：
```python
async def _handle_message(self, msg: bytes) -> None:
    frame = Frame()
    frame.ParseFromString(msg)
    ft = FrameType(frame.method)
    
    if ft == FrameType.CONTROL:
        await self._handle_control_frame(frame)
    elif ft == FrameType.DATA:
        await self._handle_data_frame(frame)
```

## 2. 我们当前的实现方式

### 2.1 核心实现路径
**文件路径**：`/home/aotao/workspace/mimiclaw/main/channels/feishu/feishu_channel.c`

### 2.2 连接流程
1. **使用固定 URL**：
   - 直接使用 `wss://open.feishu.cn/open-apis/bot/v3/hyper-event?app_id=%s`
   - 没有动态获取连接 URL 的步骤

2. **使用 tenant_access_token**：
   - 通过 HTTP POST 请求获取 `tenant_access_token`
   - 将 token 作为认证信息

3. **消息处理**：
   - 假设消息是 JSON 格式
   - 尝试解析 JSON 结构

### 2.3 存在的问题

1. **连接 URL 错误**：
   - 直接使用固定 URL，没有通过 API 获取
   - 缺少 `device_id` 和 `service_id` 等参数

2. **消息格式解析错误**：
   - 假设消息是 JSON 格式
   - 但飞书实际使用 Protocol Buffers 格式
   - JSON 解析逻辑无法处理 Protocol Buffers 数据

3. **认证方式错误**：
   - 使用 `tenant_access_token` 作为认证
   - 官方库使用 `AppID` 和 `AppSecret` 获取连接 URL

4. **缺少消息帧处理**：
   - 缺少 PING/PONG 机制
   - 缺少消息分帧和重连机制

5. **启动顺序问题**：
   - 之前在 start() 时才设置 WebSocket 回调
   - 可能导致首包丢失

## 3. 整改计划

### 3.1 短期修复
1. **修复启动顺序问题**：
   - 已完成：在 `feishu_channel_init_impl` 中设置 WebSocket 回调
   - 确保回调在连接建立之前注册

2. **修复消息解析逻辑**：
   - 修正 JSON 解析路径，适配飞书 WebSocket 消息格式

### 3.2 长期方案
1. **实现动态获取连接 URL**：
   - 添加 HTTP POST 请求到 `https://open.feishu.cn/open-apis/bot/v3/endpoints`
   - 使用 `AppID` 和 `AppSecret` 获取连接 URL

2. **添加 Protocol Buffers 支持**：
   - 集成 Protocol Buffers 库
   - 实现消息的序列化和反序列化

3. **修改认证方式**：
   - 使用 `AppID` 和 `AppSecret` 进行认证
   - 不再使用 `tenant_access_token`

4. **添加完整的消息处理**：
   - 实现 PING/PONG 机制
   - 支持消息分帧和重连

### 3.3 实现步骤

**步骤 1：添加 Protocol Buffers 依赖**
- 集成 Protocol Buffers 库
- 生成必要的 protobuf 代码

**步骤 2：实现动态连接 URL 获取**
- 添加 HTTP POST 请求逻辑
- 处理响应和错误情况

**步骤 3：修改 WebSocket 连接逻辑**
- 使用动态获取的 URL 建立连接
- 处理连接参数

**步骤 4：实现 Protocol Buffers 消息处理**
- 解析控制帧和数据帧
- 实现 PING/PONG 机制

**步骤 5：优化错误处理和重连**
- 添加重连机制
- 完善错误处理

## 4. 技术选型建议

### 4.1 Protocol Buffers 集成
- 使用 **nanopb** 库（轻量级，适合嵌入式系统）
- 或使用 **protobuf-c** 库（更完整的功能）

### 4.2 HTTP 客户端
- 使用现有的 `http_gateway` 功能
- 或集成 **libcurl** 库（更强大的 HTTP 客户端）

### 4.3 错误处理
- 实现完整的错误码处理
- 添加日志记录和监控

### 4.4 性能优化
- 使用线程池处理消息
- 实现消息队列缓冲

## 5. 预期成果

- **功能完整**：与官方库行为一致
- **稳定可靠**：支持重连和错误处理
- **性能良好**：高效处理消息
- **易于维护**：清晰的代码结构

## 6. 时间估计

| 任务 | 时间估计 |
|------|----------|
| Protocol Buffers 集成 | 2-3 天 |
| 动态连接 URL 获取 | 1-2 天 |
| 消息处理实现 | 2-3 天 |
| 测试和调优 | 2 天 |
| **总计** | **7-10 天** |

---

**文档更新日期**：2026-03-09
**文档作者**：System Generated
