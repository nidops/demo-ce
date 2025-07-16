# demo-ce

**demo-ce** is a **Zephyr-based UART Line Receiver Demo** showcasing the [CEVO Command Engine](https://github.com/nidops/cevo).
It demonstrates how to:

✅ Receive UART input using **interrupt-driven RX**
✅ Buffer complete lines in a **Zephyr message queue**
✅ Dispatch parsed commands to **CEVO demo handlers**
✅ Run on **real boards** or **QEMU**

---

## ✨ Features

- **Portable ISR-based UART line receiver**
- **Non-blocking message queue** for completed lines
- **Demo commands** for math, strings, and byte sorting
- Works on **most Zephyr boards** and **QEMU**
- Fully **Apache 2.0 licensed**

---

## ⚙️ Requirements

- [Zephyr SDK](https://docs.zephyrproject.org/latest/getting_started/index.html) ≥ 0.16
- `west` build tool
- Python ≥ 3.8

Optional:
- Real hardware (e.g. STM32 Nucleo WB55RG)
- Or simply run **QEMU**

---

## 🛠 Build & Run

### 1. Clone required repositories
```bash
cd ~/zephyrproject
git clone https://github.com/nidops/cevo.git
git clone https://github.com/nidops/demo-ce.git
```

### 2. Build for QEMU
```bash
cd demo-ce
west build -b qemu_x86 -p auto . -- -DINPUT_YAML=inputs/demo.yaml
west build -t run
```

### 3. Or build for a real board
```bash
cd demo-ce
west build -b nucleo_wb55rg -p auto . -- -DINPUT_YAML=inputs/demo.yaml
west flash
```

---

## 💻 Example UART session

When you connect via serial (or QEMU console), you’ll see:

```
UART Line Receiver + CEVO Demo ready
>>
```

Try commands:

```
>> add 10 20
Sum: 30

>> div 1000000000 4
Quotient: 250000000

>> reverse hello
olleh

>> upper helloworld
HELLOWORLD

>> sort FF01A0 3
01 A0 FF
```

Invalid commands return an error:

```
>> foobar
ERR
>>
```

---

## 📂 Project structure

```
demo-ce/
│  README.md         <-- This file
│  prj.conf          <-- Minimal Zephyr config
│
├─ src/
│   main.c           <-- UART ISR + line queue + dispatcher loop
│   demo.c           <-- Demo command handlers (add, div, reverse, etc.)
│
├─ inc/
│   uart.h           <-- UART TX helper + RX buffer definitions
│   demo.h           <-- Demo handler API
│
└─ inputs/
    demo.yaml        <-- CEVO command definition (handlers + args)
```

---

## 🧩 Command Table

The CEVO command table (`inputs/demo.yaml`) maps CLI commands to C handlers:

| Command  | Args                        | Description                     |
|----------|-----------------------------|---------------------------------|
| `add`    | `u32, u32`                  | Add two 32-bit integers         |
| `div`    | `u32, u32`                  | Divide two 32-bit integers      |
| `upper`  | `string`                    | Convert string to uppercase     |
| `reverse`| `string`                    | Reverse a string                |
| `sort`   | `u8p (bytes), u16 (length)` | Sort a hex byte array           |

---

## 🏗 Architecture Overview

```
UART ISR --> Line Buffer --> k_msgq --> Thread Mode Processing --> CEVO Dispatcher --> Handler
```

- **Interrupt level**: collects chars, detects line endings
- **Queue**: stores completed lines safely
- **Thread mode**: processes commands outside ISR
- **CEVO**: parses + validates args → calls handler

![architecture](https://dummyimage.com/800x200/000/fff&text=ISR+-->+Queue+-->+Dispatcher+-->+Handler)

---

## 📝 License

This project is licensed under the **Apache License 2.0**.
See `LICENSE` for details.

---

## 🤝 Contributing

Contributions are welcome!
