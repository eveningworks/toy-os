// sum -- read two integers from stdin and print their sum.
//
// The smallest interactive program in /bin, and it is here as one: a
// prompt with no '\n' followed by a blocking read is the shape every
// "enter a number" program has, and it is exactly the shape that was
// broken until stdio.c's refill() learned to flush stdout first.
#include <stdio.h>

int main(void)
{
    int number1 = 0, number2 = 0;

    printf("Give me two numbers: ");
    scanf("%d %d", &number1, &number2);
    printf("Sum is: %d + %d = %d\n", number1, number2, number1 + number2);
    return 0;
}
