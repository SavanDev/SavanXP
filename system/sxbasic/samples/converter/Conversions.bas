' Conversions: the module the converter's two forms share.

Public Function CelsiusToFahrenheit(celsius As Double) As Double
    Return celsius * 9 / 5 + 32
End Function

Public Function FormatTemperature(value As Double, unit As String) As String
    Return CStr(Round(value, 1)) & " " & unit
End Function
