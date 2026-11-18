' Hello: the console sample of the Savan BASIC reference (docs/userland/SXBASIC.md).
' It touches loops, functions, Select Case, strings, arrays and a caught
' runtime error. What it prints is in expected-output.txt.

Const Limit = 15

Sub Main()
    Print "Hello from Savan BASIC"

    Dim i As Integer
    Dim total As Integer
    For i = 1 To 10
        total = total + i
    Next
    Print "Sum of 1 to 10: " & total

    For i = 1 To Limit
        Print FizzBuzz(i)
    Next i

    Dim words() As String
    words = Split("one two three", " ")
    Print "Words: " & (UBound(words) + 1)
    Print "Last word: " & UCase(words(UBound(words)))

    Print "Average: " & Average(3, 4)
    Print "Grade for 7: " & Grade(7)

    Try
        Print 1 \ (total - 55)
    Catch
        Print "Error " & Err.Number & ": " & Err.Description
    End Try
End Sub

Function FizzBuzz(n As Integer) As String
    If n Mod 15 = 0 Then
        Return "FizzBuzz"
    ElseIf n Mod 3 = 0 Then
        Return "Fizz"
    ElseIf n Mod 5 = 0 Then
        Return "Buzz"
    End If
    Return CStr(n)
End Function

Function Average(a As Integer, b As Integer) As Double
    Return (a + b) / 2
End Function

Function Grade(score As Integer) As String
    Select Case score
        Case 9 To 10
            Return "A"
        Case 7, 8
            Return "B"
        Case Is >= 5
            Return "C"
        Case Else
            Return "F"
    End Select
End Function
