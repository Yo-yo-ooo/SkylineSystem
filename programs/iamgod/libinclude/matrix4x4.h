#ifndef GRAPHIC_MATRIX4X4
#define GRAPHIC_MATRIX4X4

#include <stdint.h>

class Matrix4x4;

#include <vec3d.h>
#include <vec4d.h>

class Matrix4x4
{
public:
	Matrix4x4(void);
	Matrix4x4(const float* values);
	float *data(void) {return mat;}
	const float *data(void) const {return mat;}
	const float *constData(void) const {return mat;}
	Vector4D map(const Vector4D& v) const;
	void rotate(float angle, const Vector3D& vector);
	void scale(const Vector3D& v);
	void translate(const Vector3D& v);
	void perspective(float fov, float aspect, float near, float far);
	void lookAt(const Vector3D& eye, const Vector3D& center, const Vector3D& up);

	const float& operator()(int32_t row, int32_t col) const {return *(mat + row + col * 4);}
	float& operator()(int32_t row, int32_t col) {return *(mat + row + col * 4);}
	Matrix4x4& operator*=(const Matrix4x4& m);

private:
	float mat[4 * 4];
};

const Matrix4x4 operator*(const Matrix4x4& m1, const Matrix4x4& m2);
const Vector4D operator*(const Matrix4x4& m, const Vector4D& v);

#endif