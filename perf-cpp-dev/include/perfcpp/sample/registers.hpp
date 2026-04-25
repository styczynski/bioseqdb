#pragma once

#include <bitset>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace perf {
enum class ABI : std::uint8_t
{
  None = 0U,
  Regs32 = 1U,
  Regs64 = 2U
};
class Registers
{
public:
  enum class x86 : std::uint8_t
  {
    AX = 0U,
    BX = 1U,
    CX = 2U,
    DX = 3U,
    SI = 4U,
    DI = 5U,
    BP = 6U,
    SP = 7U,
    IP = 8U,
    FLAGS = 9U,
    CS = 10U,
    SS = 11U,
    DS = 12U,
    ES = 13U,
    FS = 14U,
    GS = 15U,
    R8 = 16U,
    R9 = 17U,
    R10 = 18U,
    R11 = 19U,
    R12 = 20U,
    R13 = 21U,
    R14 = 22U,
    R15 = 23U,

    XMM0 = 32U,
    XMM1 = 34U,
    XMM2 = 36U,
    XMM3 = 38U,
    XMM4 = 40U,
    XMM5 = 42U,
    XMM6 = 44U,
    XMM7 = 46U,
    XMM8 = 48U,
    XMM9 = 50U,
    XMM10 = 52U,
    XMM11 = 54U,
    XMM12 = 56U,
    XMM13 = 58U,
    XMM14 = 60U,
    XMM15 = 62U
  };

  enum class arm : std::uint8_t
  {
    R0,
    R1,
    R2,
    R3,
    R4,
    R5,
    R6,
    R7,
    R8,
    R9,
    R10,
    FP,
    IP,
    SP,
    LR,
    PC,
    MAX
  };

  enum class arm64 : std::uint8_t
  {
    X0 = 0U,
    X1 = 1U,
    X2 = 2U,
    X3 = 3U,
    X4 = 4U,
    X5 = 5U,
    X6 = 6U,
    X7 = 7U,
    X8 = 8U,
    X9 = 9U,
    X10 = 10U,
    X11 = 11U,
    X12 = 12U,
    X13 = 13U,
    X14 = 14U,
    X15 = 15U,
    X16 = 16U,
    X17 = 17U,
    X18 = 18U,
    X19 = 19U,
    X20 = 20U,
    X21 = 21U,
    X22 = 22U,
    X23 = 23U,
    X24 = 24U,
    X25 = 25U,
    X26 = 26U,
    X27 = 27U,
    X28 = 28U,
    X29 = 29U,
    LR = 30U,
    SP = 31U,
    PC = 32U,
    MAX = 33U,
    VG = 46U
  };

  enum class riscv : std::uint8_t
  {
    PC,
    RA,
    SP,
    GP,
    TP,
    T0,
    T1,
    T2,
    S0,
    S1,
    A0,
    A1,
    A2,
    A3,
    A4,
    A5,
    A6,
    A7,
    S2,
    S3,
    S4,
    S5,
    S6,
    S7,
    S8,
    S9,
    S10,
    S11,
    T3,
    T4,
    T5,
    T6
  };

  using registers_t = std::variant<std::vector<x86>, std::vector<arm>, std::vector<arm64>, std::vector<riscv>>;

  Registers() noexcept = default;

  Registers(Registers&&) noexcept = default;
  Registers(const Registers&) = default;

  Registers& operator=(Registers&&) noexcept = default;
  Registers& operator=(const Registers&) = default;

  explicit Registers(std::vector<x86>&& registers) noexcept
    : _registers(std::move(registers))
  {
  }

  explicit Registers(std::vector<arm>&& registers) noexcept
    : _registers(std::move(registers))
  {
  }

  explicit Registers(std::vector<arm64>&& registers) noexcept
    : _registers(std::move(registers))
  {
  }

  explicit Registers(std::vector<riscv>&& registers) noexcept
    : _registers(std::move(registers))
  {
  }

  ~Registers() noexcept = default;

  [[nodiscard]] std::uint64_t mask() const
  {
    return std::visit(
      [](const auto& registers) {
        auto mask = 0ULL;
        for (const auto reg : registers) {
          mask |= static_cast<std::uint64_t>(1U) << static_cast<std::uint64_t>(reg);
        }
        return mask;
      },
      _registers);
  }

  [[nodiscard]] std::uint64_t size() const
  {
    return std::visit([](const auto& registers) { return registers.size(); }, _registers);
  }

  [[nodiscard]] bool empty() const
  {
    return std::visit([](const auto& registers) { return registers.empty(); }, _registers);
  }

  [[nodiscard]] const registers_t& registers() const noexcept { return _registers; }

private:
  registers_t _registers;
};

/**
 * Represents sampled values of general-purpose registers for a specific architecture.
 */
class RegisterValues
{
public:
  RegisterValues(const ABI abi, std::unordered_map<std::uint8_t, std::int64_t>&& register_values) noexcept
    : _abi(abi)
    , _values(std::move(register_values))
  {
  }

  explicit RegisterValues(const ABI abi)
    : _abi(abi)
  {
  }

  RegisterValues(const RegisterValues&) = default;
  RegisterValues(RegisterValues&&) noexcept = default;

  ~RegisterValues() = default;

  RegisterValues& operator=(const RegisterValues&) = default;
  RegisterValues& operator=(RegisterValues&&) noexcept = default;

  /**
   * @return The ABI for which these register values are valid.
   */
  [[nodiscard]] ABI abi() const noexcept { return _abi; }

  /**
   * @return The value of the specified x86 register, if available.
   */
  [[nodiscard]] std::optional<std::int64_t> get(const Registers::x86 reg) const noexcept
  {
    return get(static_cast<std::uint8_t>(reg));
  }

  /**
   * @return The value of the specified ARM register, if available.
   */
  [[nodiscard]] std::optional<std::int64_t> get(const Registers::arm reg) const noexcept
  {
    return get(static_cast<std::uint8_t>(reg));
  }

  /**
   * @return The value of the specified ARM64 (AArch64) register, if available.
   */
  [[nodiscard]] std::optional<std::int64_t> get(const Registers::arm64 reg) const noexcept
  {
    return get(static_cast<std::uint8_t>(reg));
  }

  /**
   * @return The value of the specified RISC-V register, if available.
   */
  [[nodiscard]] std::optional<std::int64_t> get(const Registers::riscv reg) const noexcept
  {
    return get(static_cast<std::uint8_t>(reg));
  }

  [[nodiscard]] std::optional<std::int64_t> operator[](const Registers::x86 reg) const noexcept
  {
    return get(static_cast<std::uint8_t>(reg));
  }

  [[nodiscard]] std::optional<std::int64_t> operator[](const Registers::arm reg) const noexcept
  {
    return get(static_cast<std::uint8_t>(reg));
  }

  [[nodiscard]] std::optional<std::int64_t> operator[](const Registers::arm64 reg) const noexcept
  {
    return get(static_cast<std::uint8_t>(reg));
  }

  [[nodiscard]] std::optional<std::int64_t> operator[](const Registers::riscv reg) const noexcept
  {
    return get(static_cast<std::uint8_t>(reg));
  }

private:
  /// ABI of the registers.
  ABI _abi{ ABI::None };

  /// Map of Register -> Value.
  std::unordered_map<std::uint8_t, std::int64_t> _values;

  /**
   * @return The value of the register identified by its numeric encoding, if available.
   */
  [[nodiscard]] std::optional<std::int64_t> get(const std::uint8_t reg) const noexcept
  {
    if (const auto value = _values.find(reg); value != _values.end()) {
      return value->second;
    }

    return std::nullopt;
  }
};

/**
 * Convert ABI to its string representation.
 *
 * @param abi The ABI to convert.
 * @return String representation of the ABI.
 */
[[nodiscard]] inline std::string
to_string(const ABI abi)
{
  switch (abi) {
    case ABI::None:
      return "none";
    case ABI::Regs32:
      return "32bit";
    case ABI::Regs64:
      return "64bit";
    default:
      return "unknown";
  }
}

/**
 * Convert x86 register to its string representation.
 *
 * @param reg The register to convert.
 * @return String representation of the register.
 */
[[nodiscard]] inline std::string
to_string(const Registers::x86 reg)
{
  switch (reg) {
    case Registers::x86::AX:
      return "ax";
    case Registers::x86::BX:
      return "bx";
    case Registers::x86::CX:
      return "cx";
    case Registers::x86::DX:
      return "dx";
    case Registers::x86::SI:
      return "si";
    case Registers::x86::DI:
      return "di";
    case Registers::x86::BP:
      return "bp";
    case Registers::x86::SP:
      return "sp";
    case Registers::x86::IP:
      return "ip";
    case Registers::x86::FLAGS:
      return "flags";
    case Registers::x86::CS:
      return "cs";
    case Registers::x86::SS:
      return "ss";
    case Registers::x86::DS:
      return "ds";
    case Registers::x86::ES:
      return "es";
    case Registers::x86::FS:
      return "fs";
    case Registers::x86::GS:
      return "gs";
    case Registers::x86::R8:
      return "r8";
    case Registers::x86::R9:
      return "r9";
    case Registers::x86::R10:
      return "r10";
    case Registers::x86::R11:
      return "r11";
    case Registers::x86::R12:
      return "r12";
    case Registers::x86::R13:
      return "r13";
    case Registers::x86::R14:
      return "r14";
    case Registers::x86::R15:
      return "r15";
    case Registers::x86::XMM0:
      return "xmm0";
    case Registers::x86::XMM1:
      return "xmm1";
    case Registers::x86::XMM2:
      return "xmm2";
    case Registers::x86::XMM3:
      return "xmm3";
    case Registers::x86::XMM4:
      return "xmm4";
    case Registers::x86::XMM5:
      return "xmm5";
    case Registers::x86::XMM6:
      return "xmm6";
    case Registers::x86::XMM7:
      return "xmm7";
    case Registers::x86::XMM8:
      return "xmm8";
    case Registers::x86::XMM9:
      return "xmm9";
    case Registers::x86::XMM10:
      return "xmm10";
    case Registers::x86::XMM11:
      return "xmm11";
    case Registers::x86::XMM12:
      return "xmm12";
    case Registers::x86::XMM13:
      return "xmm13";
    case Registers::x86::XMM14:
      return "xmm14";
    case Registers::x86::XMM15:
      return "xmm15";
    default:
      return "unknown";
  }
}

/**
 * Convert ARM register to its string representation.
 *
 * @param reg The register to convert.
 * @return String representation of the register.
 */
[[nodiscard]] inline std::string
to_string(const Registers::arm reg)
{
  switch (reg) {
    case Registers::arm::R0:
      return "r0";
    case Registers::arm::R1:
      return "r1";
    case Registers::arm::R2:
      return "r2";
    case Registers::arm::R3:
      return "r3";
    case Registers::arm::R4:
      return "r4";
    case Registers::arm::R5:
      return "r5";
    case Registers::arm::R6:
      return "r6";
    case Registers::arm::R7:
      return "r7";
    case Registers::arm::R8:
      return "r8";
    case Registers::arm::R9:
      return "r9";
    case Registers::arm::R10:
      return "r10";
    case Registers::arm::FP:
      return "fp";
    case Registers::arm::IP:
      return "ip";
    case Registers::arm::SP:
      return "sp";
    case Registers::arm::LR:
      return "lr";
    case Registers::arm::PC:
      return "pc";
    case Registers::arm::MAX:
      return "max";
    default:
      return "unknown";
  }
}

/**
 * Convert ARM64 register to its string representation.
 *
 * @param reg The register to convert.
 * @return String representation of the register.
 */
[[nodiscard]] inline std::string
to_string(const Registers::arm64 reg)
{
  switch (reg) {
    case Registers::arm64::X0:
      return "x0";
    case Registers::arm64::X1:
      return "x1";
    case Registers::arm64::X2:
      return "x2";
    case Registers::arm64::X3:
      return "x3";
    case Registers::arm64::X4:
      return "x4";
    case Registers::arm64::X5:
      return "x5";
    case Registers::arm64::X6:
      return "x6";
    case Registers::arm64::X7:
      return "x7";
    case Registers::arm64::X8:
      return "x8";
    case Registers::arm64::X9:
      return "x9";
    case Registers::arm64::X10:
      return "x10";
    case Registers::arm64::X11:
      return "x11";
    case Registers::arm64::X12:
      return "x12";
    case Registers::arm64::X13:
      return "x13";
    case Registers::arm64::X14:
      return "x14";
    case Registers::arm64::X15:
      return "x15";
    case Registers::arm64::X16:
      return "x16";
    case Registers::arm64::X17:
      return "x17";
    case Registers::arm64::X18:
      return "x18";
    case Registers::arm64::X19:
      return "x19";
    case Registers::arm64::X20:
      return "x20";
    case Registers::arm64::X21:
      return "x21";
    case Registers::arm64::X22:
      return "x22";
    case Registers::arm64::X23:
      return "x23";
    case Registers::arm64::X24:
      return "x24";
    case Registers::arm64::X25:
      return "x25";
    case Registers::arm64::X26:
      return "x26";
    case Registers::arm64::X27:
      return "x27";
    case Registers::arm64::X28:
      return "x28";
    case Registers::arm64::X29:
      return "x29";
    case Registers::arm64::LR:
      return "lr";
    case Registers::arm64::SP:
      return "sp";
    case Registers::arm64::PC:
      return "pc";
    case Registers::arm64::MAX:
      return "max";
    case Registers::arm64::VG:
      return "vg";
    default:
      return "unknown";
  }
}

/**
 * Convert RISC-V register to its string representation.
 *
 * @param reg The register to convert.
 * @return String representation of the register.
 */
[[nodiscard]] inline std::string
to_string(const Registers::riscv reg)
{
  switch (reg) {
    case Registers::riscv::PC:
      return "pc";
    case Registers::riscv::RA:
      return "ra";
    case Registers::riscv::SP:
      return "sp";
    case Registers::riscv::GP:
      return "gp";
    case Registers::riscv::TP:
      return "tp";
    case Registers::riscv::T0:
      return "t0";
    case Registers::riscv::T1:
      return "t1";
    case Registers::riscv::T2:
      return "t2";
    case Registers::riscv::S0:
      return "s0";
    case Registers::riscv::S1:
      return "s1";
    case Registers::riscv::A0:
      return "a0";
    case Registers::riscv::A1:
      return "a1";
    case Registers::riscv::A2:
      return "a2";
    case Registers::riscv::A3:
      return "a3";
    case Registers::riscv::A4:
      return "a4";
    case Registers::riscv::A5:
      return "a5";
    case Registers::riscv::A6:
      return "a6";
    case Registers::riscv::A7:
      return "a7";
    case Registers::riscv::S2:
      return "s2";
    case Registers::riscv::S3:
      return "s3";
    case Registers::riscv::S4:
      return "s4";
    case Registers::riscv::S5:
      return "s5";
    case Registers::riscv::S6:
      return "s6";
    case Registers::riscv::S7:
      return "s7";
    case Registers::riscv::S8:
      return "s8";
    case Registers::riscv::S9:
      return "s9";
    case Registers::riscv::S10:
      return "s10";
    case Registers::riscv::S11:
      return "s11";
    case Registers::riscv::T3:
      return "t3";
    case Registers::riscv::T4:
      return "t4";
    case Registers::riscv::T5:
      return "t5";
    case Registers::riscv::T6:
      return "t6";
    default:
      return "unknown";
  }
}

}