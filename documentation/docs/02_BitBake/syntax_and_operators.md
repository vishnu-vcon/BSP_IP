# 02. BitBake: Syntax & Operators

BitBake recipes and configuration files use a specific syntax. Understanding these operators is essential for customizing your build.

## 1. Variable Assignment Operators

| Operator | Name | Effect |
| :--- | :--- | :--- |
| `=` | **Assignment** | Immediate assignment. Overwrites previous values. |
| `?=` | **Soft Assignment** | Assigns a value only if the variable is NOT already set. |
| `??=` | **Weak Assignment** | Same as `?=`, but evaluated at the end of parsing. |
| `:=` | **Immediate Expansion** | Evaluates the value right now (like C's `#define`). |
| `+=` | **Append (with space)** | Adds text to the end with a space in between. |
| `=+` | **Prepend (with space)** | Adds text to the beginning with a space. |
| `.=` | **Append (no space)** | Adds text to the end WITHOUT a space. |
| `=.` | **Prepend (no space)** | Adds text to the beginning WITHOUT a space. |

### Example:
```python
MY_VAR = "hello"
MY_VAR += "world"  # Result: "hello world"
MY_VAR .= "!"      # Result: "hello world!"
```

---

## 2. Overrides (The Colon Syntax)

Yocto uses "overrides" to apply values only in specific contexts (like a specific machine or task).

### 2.1 Append and Prepend Overrides
These are evaluated **after** all standard assignments.

```python
# Always use the colon (:) in modern Yocto (Kirkstone and later)
SRC_URI:append = " file://my-patch.patch"
SRC_URI:prepend = "git://github.com/my-source; "
```

### 2.2 Machine-Specific Overrides
You can set a variable that only applies to one hardware type.

```python
# Only applies when MACHINE="raspberrypi4"
SERIAL_CONSOLES:raspberrypi4 = "115200;ttyS0"
```

---

## 3. Inline Python

You can use Python code directly inside variable assignments by using `${@...}`.

```python
# Get the current date in a variable
DATE = "${@time.strftime('%Y%m%d', time.gmtime())}"
```

---

## 4. Conditional Metadata (Experimental/Advanced)

You can use `if/else` logic, but it's generally discouraged in recipes. Instead, use overrides or the `PACKAGECONFIG` variable.

```python
# Bad practice:
if [ "${DEBUG}" = "1" ]; then
    ...
fi

# Good practice:
IMAGE_INSTALL:append:qemuall = " gdb"
```

> [!IMPORTANT]
> **Spacing matters!** When using `:append`, always start your string with a space (`" extra-content"`) because BitBake does not add one automatically for the append override.
