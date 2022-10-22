#include <string.h>
#include <stdio.h>

int main() {
    char str1[] = "hello hi\tmyname";
    char str2[] = "\t";

    char * ptr = strpbrk(str1, str2);

    int len = strspn(str1, str2);
    printf("%d\n", len);


    printf("%s\n", str1);
    printf("%s\n", str2);
    printf("%s\n", ptr);
}