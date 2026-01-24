#include <matrix4x4.h>
#include <libmath.h>
#include <stddef.h>
extern "C" void *IPSMemcpy(void *dest, const void *src, size_t count);

Matrix4x4::Matrix4x4(void)
{
	float values[16] = {
		1.f, 0.f, 0.f, 0.f,
		0.f, 1.f, 0.f, 0.f,
		0.f, 0.f, 1.f, 0.f,
		0.f, 0.f, 0.f, 1.f,
	};
	IPSMemcpy(mat, values, 16 * sizeof(float));
}

Vector4D Matrix4x4::map(const Vector4D& v) const
{
	Vector4D vec(0, 0, 0, 0);
	for (int32_t r = 0; r < 4; r++)
		for (int32_t i = 0; i < 4; i++)
			vec[r] += (*this)(r, i) * v[i];
	return vec;
}

void Matrix4x4::rotate(float angle, const Vector3D& vector)
{
	Vector3D v = vector.normalized();
	float c = cos(angle), s = sin(angle);
	float x = v.x(), y = v.y(), z = v.z();
	float xs = x * s, ys = y * s, zs = z * s;
	float xy1c = x * y * (1.f - c);
	float xz1c = x * z * (1.f - c);
	float yz1c = y * z * (1.f - c);
	float m[16] = {
		c + x * x * (1.f - c), xy1c + zs, xz1c - ys, 0.f,
		xy1c - zs, c + y * y * (1.f - c), yz1c + xs, 0.f,
		xz1c + ys, yz1c - xs, c + z * z * (1.f - c), 0.f,
		0.f, 0.f, 0.f, 1.f,
	};
	(*this) *= Matrix4x4(m);
}

void Matrix4x4::scale(const Vector3D& v)
{
	float m[16] = {
		v.x(), 0.f, 0.f, 0.f,
		0.f, v.y(), 0.f, 0.f,
		0.f, 0.f, v.z(), 0.f,
		0.f, 0.f, 0.f, 1.f,
	};
	(*this) *= Matrix4x4(m);
}

void Matrix4x4::translate(const Vector3D& v)
{
	float m[16] = {
		1.f, 0.f, 0.f, 0.f,
		0.f, 1.f, 0.f, 0.f,
		0.f, 0.f, 1.f, 0.f,
		v.x(), v.y(), v.z(), 1.f,
	};
	(*this) *= Matrix4x4(m);
}

void Matrix4x4::perspective(float fov, float aspect, float near, float far)
{
	float radians = fov / 2.0f * PI / 180.0f;
	float sine = sin(radians);
	float cosine = cos(radians);
	float cotan = cosine / sine;
	float clip = far - near;
	float m[16] = {
		cotan / aspect, 0.f, 0.f, 0.f,
		0.f, cotan, 0.f, 0.f,
		0.f, 0.f, -(near + far) / clip, -1.0f,
		0.f, 0.f, -(2.0f * near * far) / clip, 0.f,
	};
	(*this) *= Matrix4x4(m);
}

void Matrix4x4::lookAt(const Vector3D& eye, const Vector3D& center, const Vector3D& up)
{
	// http://www.cs.virginia.edu/~gfx/Courses/1999/intro.fall99.html/lookat.html
	Vector3D f = (center - eye).normalized();//, us = up.normalized();
	Vector3D s = Vector3D::crossProduct(f, up).normalized();
	Vector3D u = Vector3D::crossProduct(s, f);
	float m[16] = {
		s.x(), u.x(), -f.x(), 0.f,
		s.y(), u.y(), -f.y(), 0.f,
		s.z(), u.z(), -f.z(), 0.f,
		0.f, 0.f, 0.f, 1.f,
	};
	(*this) *= Matrix4x4(m);
	translate(-eye);
}

Matrix4x4& Matrix4x4::operator*=(const Matrix4x4& m)
{
	float v[16];
	for (int32_t r = 0; r < 4; r++)
		for (int32_t c = 0; c < 4; c++) {
			*(v + r + 4 * c) = 0.f;
			for (int32_t i = 0; i < 4; i++)
				*(v + r + 4 * c) += (*this)(r, i) * m(i, c);
		}
	IPSMemcpy(mat, v, 16 * sizeof(float));
	return *this;
}

const Matrix4x4 operator*(const Matrix4x4& m1, const Matrix4x4& m2)
{
	Matrix4x4 m(m1);
	m *= m2;
	return m;
}

const Vector4D operator*(const Matrix4x4& m, const Vector4D& v){
	return m.map(v);
}

Matrix4x4::Matrix4x4(const float* values)
{IPSMemcpy(mat, values, 16 * sizeof(float));}