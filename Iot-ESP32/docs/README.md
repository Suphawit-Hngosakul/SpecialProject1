# Architecture Diagrams

ไดอะแกรมสถาปัตยกรรมของระบบ ESP32 SPL + Environmental Logger สำหรับใช้ในรายงาน

| ไฟล์ | คำอธิบาย | ใช้ในบทที่ |
|---|---|---|
| [architecture.mmd](architecture.mmd) | สถาปัตยกรรมระบบโดยรวม (Edge → Cloud → 3D Web) | 3.2 |
| [hardware_block.mmd](hardware_block.mmd) | Hardware block diagram + GPIO / Bus connections | 3.2.2.1 |
| [freertos_tasks.mmd](freertos_tasks.mmd) | FreeRTOS tasks, mutexes, queues และ shared state | 3.2.2.2 |
| [sequence_dual_logging.mmd](sequence_dual_logging.mmd) | Sequence diagram ของ dual logging (SD + Cloud) | 3.2.1 / 3.4 |
| [cloud_infra.mmd](cloud_infra.mmd) | AWS deployment (EC2 + Docker + S3 + Security Group) | 3.2.1.2 |
| [web_3d_stack.mmd](web_3d_stack.mmd) | 3D Visualization frontend stack (Three.js / Cesium) | 3.2.1.2 / 3.4.3 |
| [data_schema.mmd](data_schema.mmd) | Data schema — CSV + InfluxDB measurement | 3.3 |
| [boot_state.mmd](boot_state.mmd) | Boot sequence + runtime state machine | 3.4.2 |

## วิธีเปิด / Render

- **VSCode**: ติดตั้ง extension *Markdown Preview Mermaid Support* หรือ *Mermaid Preview*
- **Online**: คัดลอกเนื้อหาไปที่ <https://mermaid.live> เพื่อ render และ export เป็น PNG / SVG
- **CLI**: `npx @mermaid-js/mermaid-cli -i architecture.mmd -o architecture.png`
