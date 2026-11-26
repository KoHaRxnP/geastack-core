import type { CheckboxProps, ClassMap, Style } from '@geastack/core'

export function Checkbox({
  checked = false,
  indeterminate = false,
  disabled = false,
  label,
  class: customClass,
  style,
  onChange,
  onPress,
  onClick,
}: CheckboxProps) {
  if (typeof label !== 'string' || label.trim().length === 0) {
    throw new Error('Checkbox requires a non-empty label.')
  }

  const handleClick = (event: Parameters<NonNullable<CheckboxProps['onClick']>>[0]) => {
    if (disabled) return

    onChange?.(indeterminate ? true : !checked)
    onPress?.(event)
    onClick?.(event)
  }

  // ClassValue is already normalized by the compiler. Merge the component's
  // state classes with the caller's string or conditional class map instead
  // of duplicating the compiler's class-resolution logic here.
  const classes: ClassMap = {
    'gea-checkbox': true,
    'gea-checkbox-disabled': disabled,
    'gea-checkbox-indeterminate': indeterminate,
    'gea-checkbox-checked': !indeterminate && checked,
    'gea-checkbox-unchecked': !indeterminate && !checked,
    ...(typeof customClass === 'string'
      ? { [customClass]: true }
      : customClass ?? {}),
  }

  const active = checked || indeterminate
  const rootStyle: Style = {
    display: 'flex',
    alignItems: 'center',
    gap: 8,
    opacity: disabled ? 0.55 : 1,
    ...style,
  }
  const boxStyle: Style = {
    width: 18,
    height: 18,
    flex: 0,
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'center',
    borderWidth: 2,
    borderColor: active ? '#2563eb' : '#64748b',
    borderRadius: 4,
    backgroundColor: active ? '#2563eb' : 'transparent',
    color: '#ffffff',
    fontSize: 12,
  }

  // One click listener handles both pointer and touch-generated click events.
  // Binding both onPress and onClick here would dispatch the same activation twice.
  return (
    <div class={classes} style={rootStyle} onClick={handleClick}>
      <span style={boxStyle}>{indeterminate ? '-' : checked ? 'x' : ''}</span>
      <span>{label}</span>
    </div>
  )
}
