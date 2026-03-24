# 微信iLink Bot API 接口协议文档

## 一、基础信息

- **Base URL**: `https://ilinkai.weixin.qq.com`
- **协议版本**: `1.0.0`
- **认证方式**: Bearer Token（登录后获取）

---

## 二、数据类型定义

### 2.1 枚举类型

#### MessageType（消息类型）
| 值 | 说明 |
|----|------|
| 0 | NONE（无） |
| 1 | USER（用户消息） |
| 2 | BOT（机器人消息） |

#### MessageItemType（消息项类型）
| 值 | 说明 |
|----|------|
| 0 | NONE（无） |
| 1 | TEXT（文本） |
| 2 | IMAGE（图片） |
| 3 | VOICE（语音） |
| 4 | FILE（文件） |
| 5 | VIDEO（视频） |

#### MessageState（消息状态）
| 值 | 说明 |
|----|------|
| 0 | NEW（新建） |
| 1 | GENERATING（生成中） |
| 2 | FINISH（完成） |

### 2.2 基础结构体

#### BaseInfo（基础信息）
```typescript
interface BaseInfo {
  channel_version?: string;  // 渠道版本，固定为 "1.0.0"
}
```

#### CDNMedia（CDN媒体信息）
```typescript
interface CDNMedia {
  encrypt_query_param?: string;  // 加密查询参数
  aes_key?: string;              // AES密钥
  encrypt_type?: number;         // 加密类型
}
```

### 2.3 消息内容结构体

#### TextItem（文本内容）
```typescript
interface TextItem {
  text?: string;  // 文本内容
}
```

#### ImageItem（图片内容）
```typescript
interface ImageItem {
  media?: CDNMedia;        // 原图媒体信息
  thumb_media?: CDNMedia;  // 缩略图媒体信息
  aeskey?: string;         // AES密钥
  url?: string;            // 图片URL
  mid_size?: number;       // 中图大小
  thumb_size?: number;     // 缩略图大小
}
```

#### VoiceItem（语音内容）
```typescript
interface VoiceItem {
  media?: CDNMedia;        // 语音媒体信息
  encode_type?: number;    // 编码类型
  sample_rate?: number;    // 采样率
  playtime?: number;       // 播放时长（秒）
  text?: string;           // 语音转文字结果
}
```

#### FileItem（文件内容）
```typescript
interface FileItem {
  media?: CDNMedia;        // 文件媒体信息
  file_name?: string;      // 文件名
  md5?: string;            // 文件MD5
  len?: string;            // 文件长度
}
```

#### VideoItem（视频内容）
```typescript
interface VideoItem {
  media?: CDNMedia;        // 视频媒体信息
  video_size?: number;     // 视频大小
  play_length?: number;    // 播放时长
}
```

#### RefMessage（引用消息）
```typescript
interface RefMessage {
  message_item?: MessageItem;  // 引用的消息项
  title?: string;              // 引用标题
}
```

#### MessageItem（消息项）
```typescript
interface MessageItem {
  type?: number;               // 消息项类型（见 MessageItemType）
  create_time_ms?: number;     // 创建时间戳（毫秒）
  msg_id?: string;             // 消息ID
  ref_msg?: RefMessage;        // 引用的消息
  text_item?: TextItem;        // 文本内容（type=1时）
  image_item?: ImageItem;      // 图片内容（type=2时）
  voice_item?: VoiceItem;      // 语音内容（type=3时）
  file_item?: FileItem;        // 文件内容（type=4时）
  video_item?: VideoItem;      // 视频内容（type=5时）
}
```

#### WeixinMessage（微信消息）
```typescript
interface WeixinMessage {
  seq?: number;                // 序列号
  message_id?: number;         // 消息ID
  from_user_id?: string;       // 发送者用户ID
  to_user_id?: string;         // 接收者用户ID
  client_id?: string;          // 客户端ID
  create_time_ms?: number;     // 创建时间戳（毫秒）
  message_type?: number;       // 消息类型（见 MessageType）
  message_state?: number;      // 消息状态（见 MessageState）
  item_list?: MessageItem[];   // 消息项列表
  context_token?: string;      // 上下文Token（用于后续交互）
}
```

---

## 三、API接口详情

### 接口1：获取登录二维码

**接口地址**: `GET /ilink/bot/get_bot_qrcode?bot_type=3`

**功能说明**: 获取用于微信扫码登录的二维码

**请求参数**:
| 参数名 | 位置 | 类型 | 必填 | 说明 |
|--------|------|------|------|------|
| bot_type | query | number | 是 | 固定值：3 |

**请求示例**:
```bash
curl "https://ilinkai.weixin.qq.com/ilink/bot/get_bot_qrcode?bot_type=3"
```

**响应类型**: `QRCodeResponse`
```typescript
interface QRCodeResponse {
  qrcode: string;              // 二维码标识（用于后续轮询）
  qrcode_img_content: string;  // 二维码图片URL
}
```

**响应示例**:
```json
{
  "qrcode": "8b37214387bf4415846239d0f780befb",
  "qrcode_img_content": "https://liteapp.weixin.qq.com/q/7GiQu1?qrcode=8b37214387bf4415846239d0f780befb&bot_type=3",
  "ret": 0
}
```

---

### 接口2：轮询二维码扫描状态

**接口地址**: `GET /ilink/bot/get_qrcode_status?qrcode={qrcode}`

**功能说明**: 轮询二维码的扫描和确认状态

**请求头**:
| 头名 | 值 | 说明 |
|------|----|------|
| iLink-App-ClientVersion | 1 | 固定值 |

**请求参数**:
| 参数名 | 位置 | 类型 | 必填 | 说明 |
|--------|------|------|------|------|
| qrcode | query | string | 是 | 从get_bot_qrcode获取的qrcode值 |

**请求示例**:
```bash
curl -H "iLink-App-ClientVersion: 1" "https://ilinkai.weixin.qq.com/ilink/bot/get_qrcode_status?qrcode=8b37214387bf4415846239d0f780befb"
```

**响应类型**: `QRStatusResponse`
```typescript
interface QRStatusResponse {
  status: "wait" | "scaned" | "confirmed" | "expired";  // 状态
  bot_token?: string;        // 机器人Token（confirmed时返回）
  ilink_bot_id?: string;     // 机器人ID（confirmed时返回）
  baseurl?: string;          // API基础URL（confirmed时返回）
  ilink_user_id?: string;    // 用户ID（confirmed时返回）
}
```

**状态说明**:
- `wait`: 等待扫描二维码
- `scaned`: 已扫描，等待确认
- `confirmed`: 已确认，登录成功
- `expired`: 二维码已过期

**响应示例**:
```json
// 等待扫描
{"ret":0,"status":"wait"}

// 登录成功
{
  "status": "confirmed",
  "bot_token": "xxxxxxxxxxxx",
  "ilink_bot_id": "xxxxxxxx",
  "ilink_user_id": "xxxxxxxx",
  "baseurl": "https://ilinkai.weixin.qq.com"
}
```

---

### 接口3：长轮询获取消息

**接口地址**: `POST /ilink/bot/getupdates`

**功能说明**: 通过长轮询获取新的微信消息

**请求头**:
| 头名 | 值 | 说明 |
|------|----|------|
| Content-Type | application/json | 请求内容类型 |
| AuthorizationType | ilink_bot_token | 认证类型 |
| Authorization | Bearer {bot_token} | Bearer认证 |
| X-WECHAT-UIN | 随机Base64字符串 | 随机用户标识 |

**请求体类型**:
```typescript
{
  get_updates_buf: string;      // 上一次返回的get_updates_buf，用于增量同步
  base_info: BaseInfo;          // 基础信息
}
```

**请求示例**:
```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -H "AuthorizationType: ilink_bot_token" \
  -H "Authorization: Bearer YOUR_BOT_TOKEN" \
  -H "X-WECHAT-UIN: MTIzNDU2Nzg5MA==" \
  -d '{"get_updates_buf":"","base_info":{"channel_version":"1.0.0"}}' \
  "https://ilinkai.weixin.qq.com/ilink/bot/getupdates"
```

**响应类型**: `GetUpdatesResp`
```typescript
interface GetUpdatesResp {
  ret?: number;                  // 返回码，0表示成功
  errcode?: number;              // 错误码
  errmsg?: string;               // 错误信息
  msgs?: WeixinMessage[];        // 消息列表
  get_updates_buf?: string;      // 增量同步缓冲区（用于下一次请求）
  longpolling_timeout_ms?: number;  // 长轮询超时时间
}
```

**响应示例**:
```json
{
  "ret": 0,
  "msgs": [
    {
      "message_id": 123456,
      "from_user_id": "user_abc123",
      "to_user_id": "bot_xyz789",
      "message_type": 1,
      "message_state": 2,
      "create_time_ms": 1732345678000,
      "item_list": [
        {
          "type": 1,
          "text_item": {
            "text": "你好，机器人！"
          }
        }
      ],
      "context_token": "context_abc123xyz"
    }
  ],
  "get_updates_buf": "next_page_token_xxx"
}
```

---

### 接口4：发送消息

**接口地址**: `POST /ilink/bot/sendmessage`

**功能说明**: 向微信用户发送消息

**请求头**: 同`getupdates`接口

**请求体类型**: `SendMessageReq`
```typescript
interface SendMessageReq {
  msg?: WeixinMessage;  // 要发送的消息
}
```

**请求字段说明**:
| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| msg.to_user_id | string | 是 | 接收者用户ID（来自getupdates的from_user_id） |
| msg.client_id | string | 是 | 客户端请求ID，格式：`前缀:时间戳-随机值` |
| msg.message_type | number | 是 | 固定值：2（BOT） |
| msg.message_state | number | 是 | 固定值：2（FINISH） |
| msg.item_list | MessageItem[] | 是 | 消息内容列表 |
| msg.context_token | string | 否 | 上下文Token（来自getupdates，用于会话连续性） |
| base_info | BaseInfo | 是 | 基础信息 |

**请求示例（发送文本）**:
```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -H "AuthorizationType: ilink_bot_token" \
  -H "Authorization: Bearer YOUR_BOT_TOKEN" \
  -H "X-WECHAT-UIN: MTIzNDU2Nzg5MA==" \
  -d '{
    "msg": {
      "to_user_id": "user_abc123",
      "client_id": "mcp-wechat:1732345678000-abc123def",
      "message_type": 2,
      "message_state": 2,
      "item_list": [
        {
          "type": 1,
          "text_item": {
            "text": "你好！我是AI助手，有什么可以帮您的？"
          }
        }
      ],
      "context_token": "context_abc123xyz"
    },
    "base_info": {
      "channel_version": "1.0.0"
    }
  }' \
  "https://ilinkai.weixin.qq.com/ilink/bot/sendmessage"
```

**响应示例**:
```json
// 成功
{"ret":0}

// 失败
{"errcode":-14,"errmsg":"session timeout"}
```

---

### 接口5：获取配置（获取Typing Ticket）

**接口地址**: `POST /ilink/bot/getconfig`

**功能说明**: 获取typing_ticket，用于发送"正在输入"状态

**请求头**: 同`getupdates`接口

**请求体类型**:
```typescript
{
  ilink_user_id: string;    // 用户ID
  context_token: string;    // 上下文Token
  base_info: BaseInfo;      // 基础信息
}
```

**请求示例**:
```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -H "AuthorizationType: ilink_bot_token" \
  -H "Authorization: Bearer YOUR_BOT_TOKEN" \
  -H "X-WECHAT-UIN: MTIzNDU2Nzg5MA==" \
  -d '{
    "ilink_user_id": "user_abc123",
    "context_token": "context_abc123xyz",
    "base_info": {"channel_version":"1.0.0"}
  }' \
  "https://ilinkai.weixin.qq.com/ilink/bot/getconfig"
```

**响应类型**: `GetConfigResp`
```typescript
interface GetConfigResp {
  ret?: number;              // 返回码
  errmsg?: string;           // 错误信息
  typing_ticket?: string;    // 用于发送输入状态的Ticket
}
```

**响应示例**:
```json
{
  "ret": 0,
  "typing_ticket": "ticket_abc123xyz789"
}
```

---

### 接口6：发送"正在输入"状态

**接口地址**: `POST /ilink/bot/sendtyping`

**功能说明**: 向用户显示或取消"正在输入..."提示

**请求头**: 同`getupdates`接口

**请求体类型**: `SendTypingReq`
```typescript
interface SendTypingReq {
  ilink_user_id?: string;    // 用户ID
  typing_ticket?: string;    // 从getconfig获取的ticket
  status?: number;           // 状态：1=正在输入，2=取消输入
  base_info?: BaseInfo;      // 基础信息
}
```

**状态值说明**:
| 值 | 说明 |
|----|------|
| 1 | 显示"正在输入..." |
| 2 | 取消"正在输入" |

**请求示例**:
```bash
# 显示正在输入
curl -X POST \
  -H "Content-Type: application/json" \
  -H "AuthorizationType: ilink_bot_token" \
  -H "Authorization: Bearer YOUR_BOT_TOKEN" \
  -H "X-WECHAT-UIN: MTIzNDU2Nzg5MA==" \
  -d '{
    "ilink_user_id": "user_abc123",
    "typing_ticket": "ticket_abc123xyz789",
    "status": 1,
    "base_info": {"channel_version":"1.0.0"}
  }' \
  "https://ilinkai.weixin.qq.com/ilink/bot/sendtyping"

# 取消正在输入
# 将 status 改为 2 即可
```

**响应示例**:
```json
{"ret":0}
```

---

## 四、错误码说明

| 错误码 | 说明 |
|--------|------|
| 0 | 成功 |
| -14 | Session超时/未登录 |
| 其他 | 见errmsg字段说明 |

---

## 五、完整交互流程

```
┌─────────────────────────────────────────────────────────────────────┐
│                        登录认证流程                                  │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  1. 客户端              GET /ilink/bot/get_bot_qrcode               │
│     ─────────────────────────────────────────────────►  服务器      │
│                                                                     │
│  2. 返回 qrcode, qrcode_img_content                                 │
│     ◄─────────────────────────────────────────────────              │
│                                                                     │
│  3. 用户使用微信扫描二维码图片                                       │
│     (离线操作)                                                      │
│                                                                     │
│  4. 循环轮询: GET /ilink/bot/get_qrcode_status                      │
│     ─────────────────────────────────────────────────►              │
│        status=wait/scaned/confirmed/expired                         │
│     ◄─────────────────────────────────────────────────              │
│                                                                     │
│  5. confirmed时获取: bot_token, ilink_bot_id, ilink_user_id         │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│                        消息交互流程                                  │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  1. 长轮询获取消息: POST /ilink/bot/getupdates                       │
│     ─────────────────────────────────────────────────►              │
│        (带 Authorization: Bearer {bot_token})                       │
│                                                                     │
│  2. 返回用户消息 msgs[]，包含:                                      │
│     - from_user_id: 发送者ID                                        │
│     - context_token: 上下文Token                                    │
│     ◄─────────────────────────────────────────────────              │
│                                                                     │
│  3. 获取Typing Ticket: POST /ilink/bot/getconfig                    │
│     (用于发送输入状态)                                              │
│     ─────────────────────────────────────────────────►              │
│     ◄─────────────────────────────────────────────────              │
│                                                                     │
│  4. 显示正在输入: POST /ilink/bot/sendtyping status=1               │
│     ─────────────────────────────────────────────────►              │
│     ◄─────────────────────────────────────────────────              │
│                                                                     │
│  5. 发送回复消息: POST /ilink/bot/sendmessage                       │
│     (to_user_id=from_user_id, context_token)                        │
│     ─────────────────────────────────────────────────►              │
│     ◄─────────────────────────────────────────────────              │
│                                                                     │
│  6. 取消正在输入: POST /ilink/bot/sendtyping status=2               │
│     ─────────────────────────────────────────────────►              │
│     ◄─────────────────────────────────────────────────              │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 六、注意事项

1. **Header生成规则**: `X-WECHAT-UIN` 是随机的32位整数经UTF-8编码后再进行Base64编码的字符串

2. **长轮询机制**: `getupdates` 接口支持长轮询，默认超时时间约35秒，无消息时会超时返回

3. **上下文保持**: `context_token` 用于维持会话上下文，回复消息时建议带上

4. **Client ID生成**: 格式为 `{prefix}:{timestamp}-{random_hex}`，如 `mcp-wechat:1732345678000-abc123def`

5. **二维码有效期**: 二维码生成后5分钟内有效，超过时间需重新获取

6. **Typing有效期**: `typing_ticket` 有效期较短，每次发送输入状态前建议重新获取
