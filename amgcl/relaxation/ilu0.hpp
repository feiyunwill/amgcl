#ifndef AMGCL_RELAXATION_ILU0_HPP
#define AMGCL_RELAXATION_ILU0_HPP

/*
The MIT License

Copyright (c) 2012-2017 Denis Demidov <dennis.demidov@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/**
 * \file   amgcl/relaxation/ilu0.hpp
 * \author Denis Demidov <dennis.demidov@gmail.com>
 * \brief  Incomplete LU with zero fill-in relaxation scheme.
 */

#include <amgcl/backend/builtin.hpp>
#include <amgcl/util.hpp>
#include <amgcl/relaxation/detail/ilu_solve.hpp>
#include <amgcl/io/mm.hpp>

namespace amgcl {
namespace relaxation {

/// ILU(0) smoother.
/**
 * \note ILU(0) is a serial algorithm and is only applicable to backends that
 * support matrix row iteration (e.g. amgcl::backend::builtin or
 * amgcl::backend::eigen).
 *
 * \param Backend Backend for temporary structures allocation.
 * \ingroup relaxation
 */
template <class Backend>
struct ilu0 {
    typedef typename Backend::value_type      value_type;
    typedef typename Backend::vector          vector;
    typedef typename Backend::matrix          matrix;
    typedef typename Backend::matrix_diagonal matrix_diagonal;

    typedef typename math::scalar_of<value_type>::type scalar_type;

    /// Relaxation parameters.
    struct params {
        /// Damping factor.
        scalar_type damping;

        /// Number of Jacobi iterations.
        /** \note Used for approximate solution of triangular systems on parallel backends */
        unsigned jacobi_iters;

        params() : damping(1), jacobi_iters(2) {}

        params(const boost::property_tree::ptree &p)
            : AMGCL_PARAMS_IMPORT_VALUE(p, damping)
            , AMGCL_PARAMS_IMPORT_VALUE(p, jacobi_iters)
        {
            AMGCL_PARAMS_CHECK(p, (damping)(jacobi_iters));
        }

        void get(boost::property_tree::ptree &p, const std::string &path) const {
            AMGCL_PARAMS_EXPORT_VALUE(p, path, damping);
            AMGCL_PARAMS_EXPORT_VALUE(p, path, jacobi_iters);
        }
    };

    /// \copydoc amgcl::relaxation::damped_jacobi::damped_jacobi
    template <class Matrix>
    ilu0( const Matrix &A, const params &, const typename Backend::params &bprm)
    {
        typedef typename backend::builtin<value_type>::matrix build_matrix;
        const size_t n = backend::rows(A);

        size_t Lnz = 0, Unz = 0;

        for(ptrdiff_t i = 0; i < static_cast<ptrdiff_t>(n); ++i) {
            ptrdiff_t row_beg = A.ptr[i];
            ptrdiff_t row_end = A.ptr[i + 1];

            for(ptrdiff_t j = row_beg; j < row_end; ++j) {
                ptrdiff_t c = A.col[j];
                if (c < i)
                    ++Lnz;
                else if (c > i)
                    ++Unz;
            }
        }

        boost::shared_ptr<build_matrix> L = boost::make_shared<build_matrix>();
        boost::shared_ptr<build_matrix> U = boost::make_shared<build_matrix>();

        L->set_size(n, n); L->set_nonzeros(Lnz); L->ptr[0] = 0;
        U->set_size(n, n); U->set_nonzeros(Unz); U->ptr[0] = 0;

        size_t Lhead = 0;
        size_t Uhead = 0;

        std::vector<value_type> D;
        D.reserve(n);

        std::vector<value_type*> work(n, NULL);

        for(ptrdiff_t i = 0; i < static_cast<ptrdiff_t>(n); ++i) {
            ptrdiff_t row_beg = A.ptr[i];
            ptrdiff_t row_end = A.ptr[i + 1];

            for(ptrdiff_t j = row_beg; j < row_end; ++j) {
                ptrdiff_t  c = A.col[j];
                value_type v = A.val[j];

                if (c < i) {
                    L->col[Lhead] = c;
                    L->val[Lhead] = v;
                    work[c] = L->val + Lhead;
                    ++Lhead;
                } else if (c == i) {
                    D.push_back(v);
                    work[c] = &D.back();
                } else {
                    U->col[Uhead] = c;
                    U->val[Uhead] = v;
                    work[c] = U->val + Uhead;
                    ++Uhead;
                }
            }

            L->ptr[i+1] = Lhead;
            U->ptr[i+1] = Uhead;

            for(ptrdiff_t j = row_beg; j < row_end; ++j) {
                ptrdiff_t c = A.col[j];

                // Exit if diagonal is reached
                if (c >= i) {
                    precondition(c == i, "No diagonal value in system matrix");
                    precondition(!math::is_zero(D[i]), "Zero pivot in ILU");

                    D[i] = math::inverse(D[i]);
                    break;
                }

                // Compute the multiplier for jrow
                value_type tl = (*work[c]) * D[c];
                *work[c] = tl;

                // Perform linear combination
                for(ptrdiff_t k = U->ptr[c]; k < U->ptr[c+1]; ++k) {
                    value_type *w = work[U->col[k]];
                    if (w) *w -= tl * U->val[k];
                }
            }

            // Refresh work
            for(ptrdiff_t j = row_beg; j < row_end; ++j)
                work[A.col[j]] = NULL;
        }

        io::mm_write("ilu_d.mtx", D.data(), n);
        io::mm_write("ilu_l.mtx", *L);
        io::mm_write("ilu_u.mtx", *U);

        this->D = Backend::copy_vector(D, bprm);
        this->L = Backend::copy_matrix(L, bprm);
        this->U = Backend::copy_matrix(U, bprm);

        if (!serial_backend::value) {
            t1 = Backend::create_vector(n, bprm);
            t2 = Backend::create_vector(n, bprm);
        }
    }

    /// \copydoc amgcl::relaxation::damped_jacobi::apply_pre
    template <class Matrix, class VectorRHS, class VectorX, class VectorTMP>
    void apply_pre(
            const Matrix &A, const VectorRHS &rhs, VectorX &x, VectorTMP &tmp,
            const params &prm
            ) const
    {
        backend::residual(rhs, A, x, tmp);
        solve(tmp, prm, serial_backend());
        backend::axpby(prm.damping, tmp, math::identity<scalar_type>(), x);
    }

    /// \copydoc amgcl::relaxation::damped_jacobi::apply_post
    template <class Matrix, class VectorRHS, class VectorX, class VectorTMP>
    void apply_post(
            const Matrix &A, const VectorRHS &rhs, VectorX &x, VectorTMP &tmp,
            const params &prm
            ) const
    {
        backend::residual(rhs, A, x, tmp);
        solve(tmp, prm, serial_backend());
        backend::axpby(prm.damping, tmp, math::identity<scalar_type>(), x);
    }

    /// \copydoc amgcl::relaxation::damped_jacobi::apply_post
    template <class Matrix, class VectorRHS, class VectorX>
    void apply(const Matrix&, const VectorRHS &rhs, VectorX &x, const params &prm) const
    {
        backend::copy(rhs, x);
        solve(x, prm, serial_backend());
    }

    private:
        typedef typename boost::is_same<
                Backend, backend::builtin<value_type>
            >::type serial_backend;

        boost::shared_ptr<matrix> L, U;
        boost::shared_ptr<matrix_diagonal> D;
        boost::shared_ptr<vector> t1, t2;

        template <class VectorX>
        void solve(VectorX &x, const params&, boost::true_type) const
        {
            relaxation::detail::serial_ilu_solve(*L, *U, *D, x);
        }

        template <class VectorX>
        void solve(VectorX &x, const params &prm, boost::false_type) const
        {
            relaxation::detail::parallel_ilu_solve(
                    *L, *U, *D, x, *t1, *t2, prm.jacobi_iters);
        }

};

} // namespace relaxation
} // namespace amgcl



#endif
